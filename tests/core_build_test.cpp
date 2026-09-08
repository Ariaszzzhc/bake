#include <kota/async/io/watcher.h>
#include <kota/async/runtime/task.h>

import std;
import bake.json;
import bake.util;
import bake.toolchain.target;
import bake.buildsystem.project;
import bake.buildsystem.cmdgen;
import bake.buildsystem.graph;
import bake.buildsystem.plan;
import bake.core.model;
import bake.core.snapshot;
import bake.core.project;
import bake.core.runtime;
import bake.core.state;
import bake.core.build;

namespace {

std::string executable_path;
std::string bake_path;
int checks = 0;

void check(bool condition, std::string_view message) {
    ++checks;
    if (!condition) throw std::runtime_error(std::string(message));
}

std::string discover_bake() {
    if (const char* configured = std::getenv("BAKE_TEST_BAKE"))
        return configured;
    if (const char* configured = std::getenv("BAKE_EXE"))
        return configured;
    auto directory = std::filesystem::current_path();
    for (int depth = 0; depth < 8; ++depth) {
        if (std::filesystem::is_regular_file(directory / "bake/src/main.cpp")) {
            auto candidate = directory / "out" /
                bake::detect_host_target().triple_with_version() /
                "dev/bin/bake";
            if (std::filesystem::is_regular_file(candidate))
                return candidate.string();
        }
        if (!directory.has_parent_path() || directory == directory.parent_path())
            break;
        directory = directory.parent_path();
    }
    return {};
}

struct Fixture {
    bake::Path root;
    bake::ProjectHandle handle;

    Fixture() {
        root = bake::Path(std::filesystem::temp_directory_path() /
            ("bake-core-build-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count())));
        (root / "src").mkdir_recursive();
        bake::write_file(root / "src" / "first.c", "FIRST-v1\n");
        bake::write_file(root / "src" / "second.c", "SECOND-v1\n");
        root = bake::Path(std::filesystem::canonical(root.fs()));

        bake::CompilerIdentity compiler{
            bake::Path(std::filesystem::canonical(executable_path)),
            "core-build-test", bake::SHA256::hex_file(bake::Path(executable_path))};
        auto context = bake::make_project_context(
            root, std::nullopt, {}, "dev",
            bake::parse_target("x86_64-linux-gnu.2.28"), compiler);
        if (!context) throw std::runtime_error(context.error().message);
        auto key = bake::context_key(*context);
        auto paths = bake::context_paths(*context);
        if (!key || !paths) throw std::runtime_error("invalid test context");

        bake::BuildGraph graph;
        graph.project_root = root;
        graph.state_dir = paths->bake;
        graph.actions = {
            action("compile:first", "first.c", root / "src" / "first.c",
                   paths->object / "first.o"),
            action("compile:second", "second.c", root / "src" / "second.c",
                   paths->object / "second.o"),
        };
        bake::BuildAction link;
        link.type = bake::BuildAction::Type::Link;
        link.id = "link:app";
        link.moid_id = "root";
        link.moid = "app";
        link.description = "link app";
        link.inputs = {paths->object / "first.o", paths->object / "second.o"};
        link.outputs = {paths->bin / "app"};
        link.depends_on = {0, 1};
        link.command = {executable_path, "--child", "link",
                        link.inputs[0].string(), link.inputs[1].string(),
                        "-o", link.outputs[0].string()};
        graph.actions.push_back(std::move(link));

        auto snapshot = std::make_shared<bake::ProjectSnapshot>();
        snapshot->context = *context;
        snapshot->declaration_inputs = {*key, {}};
        snapshot->build_graph = std::move(graph);
        handle.context = *context;
        handle.paths = *paths;
        handle.snapshot_id = {bake::SHA256::hex("core-build-test-snapshot")};
        handle.snapshot = std::move(snapshot);
    }

    ~Fixture() {
        std::error_code ignored;
        std::filesystem::remove_all(root.fs(), ignored);
    }

    bake::BuildAction action(std::string id, std::string description,
                             const bake::Path& input,
                             const bake::Path& output) const {
        bake::BuildAction action;
        action.type = bake::BuildAction::Type::Compile;
        action.id = std::move(id);
        action.moid_id = "root";
        action.moid = "app";
        action.description = std::move(description);
        action.inputs = {input};
        action.outputs = {output};
        action.command = {executable_path, "--child", "compile",
                          input.string(), "-o", output.string()};
        return action;
    }
};

std::string overlaid_path(std::string original, const std::string& overlay) {
    if (overlay.empty()) return original;
    auto content = bake::read_file(bake::Path(overlay));
    if (!content) return original;
    auto document = bake::json::Value::parse(*content);
    for (const auto& entry : document["roots"]) {
        if (entry.value("name", "") == original)
            return entry.value("external-contents", original);
    }
    return original;
}

int child_main(int argc, char** argv) {
    if (argc < 6) return 2;
    std::string mode = argv[2];
    auto output_at = std::find_if(argv + 3, argv + argc,
        [](const char* value) { return std::string_view(value) == "-o"; });
    if (output_at == argv + argc || output_at + 1 == argv + argc) return 2;
    bake::Path output(*(output_at + 1));
    std::string overlay;
    auto overlay_at = std::find_if(output_at + 2, argv + argc,
        [](const char* value) {
            return std::string_view(value) == "-ivfsoverlay";
        });
    if (overlay_at != argv + argc && overlay_at + 1 != argv + argc)
        overlay = *(overlay_at + 1);

    std::string product;
    for (auto input = argv + 3; input != output_at; ++input) {
        auto content = bake::read_file(
            bake::Path(overlaid_path(*input, overlay)));
        if (!content) return 3;
        product += *content;
    }
    if (product.contains("SLOW"))
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (product.contains("FAIL")) return 9;
    output.parent().mkdir_recursive();
    if (!bake::write_file(output, mode + ":" + product)) return 4;
    auto also_output = std::find_if(output_at + 2, argv + argc,
        [](const char* value) {
            return std::string_view(value) == "--also-output";
        });
    if (also_output != argv + argc && also_output + 1 != argv + argc) {
        bake::Path second_output(*(also_output + 1));
        second_output.parent().mkdir_recursive();
        if (!bake::write_file(second_output, mode + ":also:" + product))
            return 4;
    }
    return 0;
}

std::vector<bool> reused_actions(const Fixture& fixture,
                                 const bake::BuildRequestId& request) {
    auto key = bake::context_key(fixture.handle.context);
    bake::ProjectStateStore store(fixture.handle.paths, *key);
    auto result = store.load_request_result(request);
    if (!result) throw std::runtime_error(result.error().message);
    std::vector<bool> reused;
    for (const auto& action : result->payload["actions"])
        reused.push_back(action.value("reused", false));
    return reused;
}

std::string artifact_digest(const bake::BuildResult& result,
                            std::string_view name) {
    const auto* ready = std::get_if<bake::ReadyBuild>(&result.outcome);
    if (!ready) return {};
    for (const auto& artifact : ready->artifacts)
        if (artifact.output_name == name) return artifact.digest;
    return {};
}

kota::task<bool> staged_success_case() {
    Fixture fixture;
    auto& loop = kota::event_loop::current();
    bake::CoreRuntime runtime(loop, {.max_concurrency = 2,
                                     .background_concurrency = 2});
    bake::CoreBuildService builds(runtime);
    auto first = builds.start(fixture.handle,
                              {.request = {"staged-first"}});
    check(first.has_value(), "first build did not start");
    auto active_collision = builds.start(
        fixture.handle, {.request = {"staged-first"}});
    check(!active_collision.has_value(),
          "active request identity reused the same work directory");
    check(!(fixture.handle.paths.bin / "app").exists(),
          "final output existed before publication");
    auto first_result = co_await first->wait();
    check(bake::status_of(first_result.outcome) == bake::ResultStatus::ready,
          "staged build did not publish ready");
    check(builds.validate_ready(fixture.handle, first_result).has_value(),
          "published artifact digest did not validate");
    check(artifact_digest(first_result, "app") ==
              bake::SHA256::hex_file(fixture.handle.paths.bin / "app"),
          "BuildResult digest differs from the final artifact");
    auto completed_collision = builds.start(
        fixture.handle, {.request = {"staged-first"}});
    check(!completed_collision.has_value(),
          "completed request identity was accepted again");

    auto touched = fixture.root / "src" / "first.c";
    auto old_time = std::filesystem::last_write_time(touched.fs());
    std::filesystem::last_write_time(touched.fs(),
                                     old_time + std::chrono::seconds(1));
    auto second = builds.start(fixture.handle,
                               {.request = {"same-content-touch"}});
    check(second.has_value(), "touch build did not start");
    auto second_result = co_await second->wait();
    check(bake::status_of(second_result.outcome) == bake::ResultStatus::ready,
          "same-content touch did not remain ready");
    check(reused_actions(fixture, second_result.request) ==
              std::vector<bool>({true, true, true}),
          "same-content touch rebuilt an action");

    bake::write_file(touched, "FIRST-v2\n");
    std::filesystem::last_write_time(touched.fs(), old_time);
    auto third = builds.start(fixture.handle,
                              {.request = {"same-mtime-change"}});
    check(third.has_value(), "content-change build did not start");
    auto third_result = co_await third->wait();
    check(bake::status_of(third_result.outcome) == bake::ResultStatus::ready,
          "same-mtime content change did not publish");
    check(reused_actions(fixture, third_result.request) ==
              std::vector<bool>({false, true, false}),
          "content change did not rebuild only the affected closure");
    auto linked = bake::read_file(fixture.handle.paths.bin / "app");
    check(linked && linked->contains("SECOND-v1"),
          "dirty consumer did not use the validated reused producer");
    co_await runtime.shutdown();
    co_return true;
}

kota::task<bool> stale_kill_race_case() {
    Fixture fixture;
    auto& loop = kota::event_loop::current();
    bake::CoreRuntime runtime(loop, {.max_concurrency = 2,
                                     .background_concurrency = 2});
    bake::CoreBuildService builds(runtime);
    auto baseline = builds.start(fixture.handle,
                                 {.request = {"race-baseline"}});
    auto baseline_result = co_await baseline->wait();
    check(bake::status_of(baseline_result.outcome) == bake::ResultStatus::ready,
          "race baseline failed");
    const auto baseline_digest = artifact_digest(baseline_result, "app");

    auto first_object = fixture.handle.paths.object / "first.o";
    auto first_object_content = bake::read_file(first_object);
    check(first_object_content.has_value(), "baseline object was missing");
    bake::write_file(fixture.root / "src" / "second.c", "SLOW-v2\n");
    auto reused_race = builds.start(
        fixture.handle, {.request = {"race-reused-artifact"}});
    check(reused_race.has_value(), "reused artifact race did not start");
    co_await kota::sleep(20, loop);
    bake::write_file(first_object, "foreign-object\n");
    auto reused_race_result = co_await reused_race->wait();
    check(bake::status_of(reused_race_result.outcome) ==
              bake::ResultStatus::cancelled,
          "replaced reused artifact produced a mixed ready result");
    check(bake::SHA256::hex_file(fixture.handle.paths.bin / "app") ==
              baseline_digest,
          "reused artifact race replaced the prior terminal output");
    bake::write_file(first_object, *first_object_content);
    bake::write_file(fixture.root / "src" / "second.c", "SECOND-v1\n");

    auto reused_cancel = builds.start(
        fixture.handle, {.request = {"race-reused-cancel"}});
    check(reused_cancel.has_value(), "reused cancellation did not start");
    reused_cancel->cancel();
    auto reused_cancel_result = co_await reused_cancel->wait();
    check(bake::status_of(reused_cancel_result.outcome) ==
              bake::ResultStatus::cancelled,
          "all-reused cancelled request published success");

    auto source = fixture.root / "src" / "first.c";
    bake::write_file(source, "FAST-new\n");
    auto completed_cancel = builds.start(
        fixture.handle, {.request = {"race-completed-cancel"}});
    check(completed_cancel.has_value(), "completed cancellation did not start");
    co_await kota::sleep(100, loop);
    completed_cancel->cancel();
    auto completed_cancel_result = co_await completed_cancel->wait();
    check(bake::status_of(completed_cancel_result.outcome) ==
              bake::ResultStatus::cancelled,
          "completed actions published after operation cancellation");
    check(bake::SHA256::hex_file(fixture.handle.paths.bin / "app") ==
              baseline_digest,
          "cancelled completed actions replaced prior output");

    bake::write_file(source, "SLOW-old\n");
    auto stale = builds.start(fixture.handle,
                              {.request = {"race-stale"}});
    check(stale.has_value(), "stale request did not start");
    co_await kota::sleep(20, loop);
    bake::write_file(source, "NEWER-v2\n");
    auto stale_result = co_await stale->wait();
    check(bake::status_of(stale_result.outcome) ==
              bake::ResultStatus::cancelled,
          "changed input request published success");
    check(bake::SHA256::hex_file(fixture.handle.paths.bin / "app") ==
              baseline_digest,
          "stale request replaced the prior ready output");

    bake::write_file(source, "SLOW-new\n");
    auto killed = builds.start(fixture.handle,
                               {.request = {"race-killed"}});
    check(killed.has_value(), "kill request did not start");
    co_await kota::sleep(20, loop);
    killed->cancel();
    auto killed_result = co_await killed->wait();
    check(bake::status_of(killed_result.outcome) ==
              bake::ResultStatus::cancelled,
          "killed request published success");
    check(bake::SHA256::hex_file(fixture.handle.paths.bin / "app") ==
              baseline_digest,
          "killed staging request replaced the prior output");
    co_await runtime.shutdown();
    co_return true;
}

kota::task<bool> publication_atomicity_case() {
    Fixture fixture;
    auto snapshot = std::make_shared<bake::ProjectSnapshot>(
        *fixture.handle.snapshot);
    const auto first = fixture.handle.paths.bin / "atomic-first";
    const auto created = fixture.handle.paths.bin / "atomic-created";
    const auto missing = fixture.handle.paths.bin / "atomic-missing";
    bake::BuildAction action;
    action.type = bake::BuildAction::Type::Link;
    action.id = "link:atomic-pair";
    action.moid_id = "root";
    action.moid = "atomic-pair";
    action.description = "publish an atomic output pair";
    action.inputs = {fixture.root / "src" / "first.c"};
    action.outputs = {first, created, missing};
    action.command = {executable_path, "--child", "link",
                      action.inputs.front().string(), "-o", first.string(),
                      "--also-output", created.string()};
    snapshot->build_graph.actions = {std::move(action)};
    fixture.handle.snapshot = std::move(snapshot);
    fixture.handle.snapshot_id = {
        bake::SHA256::hex("core-build-publication-atomicity")};
    bake::write_file(first, "prior-first\n");

    auto& loop = kota::event_loop::current();
    bake::CoreRuntime runtime(loop, {.max_concurrency = 1,
                                     .background_concurrency = 1});
    bake::CoreBuildService builds(runtime);
    auto operation = builds.start(
        fixture.handle, {.request = {"publication-atomicity"}});
    check(operation.has_value(), "atomic publication build did not start");
    auto result = co_await operation->wait();
    check(bake::status_of(result.outcome) == bake::ResultStatus::failed,
          "missing second output did not fail publication");
    check(bake::read_file(first) == std::optional<std::string>("prior-first\n"),
          "failed publication replaced its first prior output");
    check(!created.exists(),
          "failed publication retained a newly created output");
    check(!missing.exists(),
          "failed publication materialized its missing output");
    check(!builds.validate_ready(fixture.handle, result).has_value(),
          "failed publication validated as ready");
    co_await runtime.shutdown();
    co_return true;
}

kota::task<bool> independent_insertion_case() {
    Fixture fixture;
    auto& loop = kota::event_loop::current();
    bake::CoreRuntime runtime(loop, {.max_concurrency = 2,
                                     .background_concurrency = 2});
    bake::CoreBuildService builds(runtime);
    auto baseline = builds.start(
        fixture.handle, {.request = {"insertion-baseline"}});
    check(baseline.has_value(), "insertion baseline did not start");
    auto baseline_result = co_await baseline->wait();
    check(bake::status_of(baseline_result.outcome) == bake::ResultStatus::ready,
          "insertion baseline failed");

    auto snapshot = std::make_shared<bake::ProjectSnapshot>(
        *fixture.handle.snapshot);
    auto original = std::exchange(
        snapshot->build_graph.actions, std::vector<bake::BuildAction>());
    auto independent = fixture.action(
        "compile:independent", "independent.c",
        fixture.root / "src" / "first.c",
        fixture.handle.paths.object / "independent.o");
    snapshot->build_graph.actions.push_back(std::move(independent));
    for (auto& action : original) {
        for (auto& dependency : action.depends_on) ++dependency;
        snapshot->build_graph.actions.push_back(std::move(action));
    }
    fixture.handle.snapshot = std::move(snapshot);
    fixture.handle.snapshot_id = {
        bake::SHA256::hex("core-build-independent-insertion")};
    auto inserted = builds.start(
        fixture.handle, {.request = {"insertion-updated"}});
    check(inserted.has_value(), "independent insertion build did not start");
    auto inserted_result = co_await inserted->wait();
    check(bake::status_of(inserted_result.outcome) == bake::ResultStatus::ready,
          "independent insertion build failed");
    check(reused_actions(fixture, inserted_result.request) ==
              std::vector<bool>({false, true, true, true}),
          "action index shifts invalidated stable prior receipts");
    co_await runtime.shutdown();
    co_return true;
}

kota::task<bool> selected_capture_case() {
    Fixture fixture;
    auto snapshot = std::make_shared<bake::ProjectSnapshot>(
        *fixture.handle.snapshot);
    const auto unrelated = fixture.handle.paths.bin / "unrelated";
    snapshot->build_graph.actions.push_back(fixture.action(
        "compile:unrelated", "unrelated",
        fixture.root / "src" / "first.c", unrelated));
    fixture.handle.snapshot = std::move(snapshot);
    fixture.handle.snapshot_id = {
        bake::SHA256::hex("core-build-selected-capture")};

    auto& loop = kota::event_loop::current();
    bake::CoreRuntime runtime(loop, {.max_concurrency = 2,
                                     .background_concurrency = 2});
    bake::CoreBuildService builds(runtime);
    auto operation = builds.start(
        fixture.handle, {.request = {"selected-capture"}});
    check(operation.has_value(), "selected capture build did not start");
    auto result = co_await operation->wait();
    check(bake::status_of(result.outcome) == bake::ResultStatus::ready,
          "selected capture build failed");
    const auto& artifacts = std::get<bake::ReadyBuild>(result.outcome).artifacts;
    auto selected = std::ranges::find_if(
        artifacts, [&](const bake::ArtifactIdentity& artifact) {
            return artifact.path == fixture.handle.paths.bin / "app";
        });
    check(selected != artifacts.end(),
          "selected capture result omitted its executable");

    bake::write_file(unrelated, "unrelated-corruption\n");
    check(!builds.validate_ready(fixture.handle, result),
          "full validation ignored unrelated artifact corruption");
    bake::Path capture_root;
    {
        auto capture = builds.capture_execution(
            fixture.handle, result, *selected);
        check(capture.has_value(),
              "unrelated artifact corruption blocked selected capture");
        capture_root = capture->executable().parent().parent();
    }
    check(!capture_root.exists(),
          "selected execution capture was not cleaned");
    bake::write_file(selected->path, "selected-corruption\n");
    check(!builds.capture_execution(fixture.handle, result, *selected),
          "selected artifact corruption passed execution capture");
    co_await runtime.shutdown();
    co_return true;
}

kota::task<bool> module_relocation_case() {
    check(!bake_path.empty(), "BAKE_TEST_BAKE is required for module relocation");
    Fixture fixture;
    bake::Path driver(std::filesystem::canonical(bake_path));
    bake::CompilerIdentity compiler{
        driver, "core-build-module-test", bake::SHA256::hex_file(driver)};
    auto context = bake::make_project_context(
        fixture.root, std::nullopt, {}, "dev", bake::detect_host_target(),
        compiler);
    check(context.has_value(), "real module context was invalid");
    auto paths = bake::context_paths(*context);
    check(paths.has_value(), "real module paths were invalid");
    auto context_identity = bake::context_key(*context);

    auto a_source = fixture.root / "src" / "a.cppm";
    auto b_source = fixture.root / "src" / "b.cppm";
    auto main_source = fixture.root / "src" / "main.cpp";
    bake::write_file(a_source,
                     "export module a; export int a() { return 1; }\n");
    bake::write_file(b_source,
                     "export module b; import a; "
                     "export int b() { return a(); }\n");
    bake::write_file(main_source,
                     "import b; int main() { return b() == 1 ? 0 : 1; }\n");

    auto a_object = paths->object / "a.o";
    auto a_bmi = paths->bmi / "a.pcm";
    auto b_object = paths->object / "b.o";
    auto b_bmi = paths->bmi / "b.pcm";
    auto main_object = paths->object / "main.o";
    auto executable = paths->bin / "module-app";

    bake::BuildAction a;
    a.type = bake::BuildAction::Type::CompileModule;
    a.id = "module:a";
    a.moid_id = "root";
    a.moid = "module-app";
    a.inputs = {a_source};
    a.outputs = {a_object, a_bmi};
    a.command = {driver.string(), "c++", "-c", "-std=c++20", "-x",
                 "c++-module", "-fmodule-output=" + a_bmi.string(),
                 a_source.string(), "-o", a_object.string()};

    bake::BuildAction b;
    b.type = bake::BuildAction::Type::CompileModule;
    b.id = "module:b";
    b.moid_id = "root";
    b.moid = "module-app";
    b.inputs = {b_source, a_bmi};
    b.outputs = {b_object, b_bmi};
    b.depends_on = {0};
    b.command = {driver.string(), "c++", "-c", "-std=c++20", "-x",
                 "c++-module", "-fmodule-file=a=" + a_bmi.string(),
                 "-fmodule-output=" + b_bmi.string(), b_source.string(),
                 "-o", b_object.string()};

    bake::BuildAction main;
    main.type = bake::BuildAction::Type::Compile;
    main.id = "compile:main";
    main.moid_id = "root";
    main.moid = "module-app";
    main.inputs = {main_source, a_bmi, b_bmi};
    main.outputs = {main_object};
    main.depends_on = {0, 1};
    main.command = {driver.string(), "c++", "-c", "-std=c++20",
                    "-fmodule-file=a=" + a_bmi.string(),
                    "-fmodule-file=b=" + b_bmi.string(),
                    main_source.string(), "-o", main_object.string()};

    bake::BuildAction link;
    link.type = bake::BuildAction::Type::Link;
    link.id = "link:module-app";
    link.moid_id = "root";
    link.moid = "module-app";
    link.inputs = {a_object, b_object, main_object};
    link.outputs = {executable};
    link.depends_on = {0, 1, 2};
    link.command = {driver.string(), "c++", a_object.string(),
                    b_object.string(), main_object.string(), "-o",
                    executable.string()};

    bake::BuildGraph graph;
    graph.project_root = fixture.root;
    graph.state_dir = paths->bake;
    graph.actions = {std::move(a), std::move(b), std::move(main),
                     std::move(link)};
    auto snapshot = std::make_shared<bake::ProjectSnapshot>();
    snapshot->context = *context;
    snapshot->declaration_inputs = {*context_identity, {}};
    snapshot->build_graph = std::move(graph);
    fixture.handle.context = *context;
    fixture.handle.paths = *paths;
    fixture.handle.snapshot_id = {
        bake::SHA256::hex("core-build-module-snapshot")};
    fixture.handle.snapshot = std::move(snapshot);

    auto& loop = kota::event_loop::current();
    bake::CoreRuntime runtime(loop, {.max_concurrency = 2,
                                     .background_concurrency = 2});
    bake::CoreBuildService builds(runtime);
    auto first = builds.start(fixture.handle,
                              {.request = {"module-first"}});
    check(first.has_value(), "module build did not start");
    auto first_result = co_await first->wait();
    check(bake::status_of(first_result.outcome) == bake::ResultStatus::ready,
          "module build failed");
    std::error_code remove_error;
    std::filesystem::remove_all(
        (paths->work / first_result.request.value).fs(), remove_error);
    check(!remove_error, "completed module staging could not be removed");

    bake::write_file(main_source,
                     "import b;\nint main() { return b() == 1 ? 0 : 1; }\n");
    auto second = builds.start(fixture.handle,
                               {.request = {"module-consumer-change"}});
    check(second.has_value(), "module consumer rebuild did not start");
    auto second_result = co_await second->wait();
    check(bake::status_of(second_result.outcome) == bake::ResultStatus::ready,
          "promoted BMI depended on removed staging paths");
    check(reused_actions(fixture, second_result.request) ==
              std::vector<bool>({true, true, false, false}),
          "module consumer rebuild did not reuse promoted BMIs");
    auto run = bake::run_process({executable.string()}, fixture.root);
    check(run.success(), "module executable did not run successfully");
    co_await runtime.shutdown();
    co_return true;
}

kota::task<bool> dylib_relocation_case() {
    check(!bake_path.empty(), "BAKE_TEST_BAKE is required for dylib relocation");
    Fixture fixture;
    bake::Path driver(std::filesystem::canonical(bake_path));
    auto target = bake::detect_host_target();
    check(!target.is_windows(),
          "dylib relocation contract requires a Darwin or ELF host");
    bake::CompilerIdentity compiler{
        driver, "core-build-dylib-test", bake::SHA256::hex_file(driver)};
    auto context = bake::make_project_context(
        fixture.root, std::nullopt, {}, "dev", target, compiler);
    check(context.has_value(), "real dylib context was invalid");
    auto paths = bake::context_paths(*context);
    auto context_identity = bake::context_key(*context);
    check(paths.has_value() && context_identity.has_value(),
          "real dylib paths were invalid");

    auto library_source = fixture.root / "src" / "library.cpp";
    auto main_source = fixture.root / "src" / "dylib-main.cpp";
    bake::write_file(library_source,
                     "extern \"C\" int probe_value() { return 42; }\n");
    bake::write_file(main_source,
                     "extern \"C\" int probe_value(); "
                     "int main() { return probe_value() == 42 ? 0 : 1; }\n");
    auto library_object = paths->object / "library.o";
    auto main_object = paths->object / "dylib-main.o";
    auto library = paths->lib /
        bake::library_name("probe", bake::MoidType::Dylib, target);
    auto executable = paths->bin /
        bake::library_name("dylib-probe", bake::MoidType::Executable, target);

    bake::BuildAction compile_library;
    compile_library.type = bake::BuildAction::Type::Compile;
    compile_library.id = "compile:dylib-library";
    compile_library.moid_id = "library";
    compile_library.moid = "probe";
    compile_library.inputs = {library_source};
    compile_library.outputs = {library_object};
    compile_library.command = {driver.string(), "c++", "-c", "-std=c++20",
                               "-fPIC", library_source.string(), "-o",
                               library_object.string()};

    bake::BuildAction link_library;
    link_library.type = bake::BuildAction::Type::Link;
    link_library.id = "link:dylib-library";
    link_library.moid_id = "library";
    link_library.moid = "probe";
    link_library.inputs = {library_object};
    link_library.outputs = {library};
    link_library.depends_on = {0};
    bake::LinkCommand library_link;
    library_link.objects = {library_object};
    library_link.output = library;
    library_link.type = bake::MoidType::Dylib;
    link_library.command = bake::make_link_command(target, library_link);
    link_library.command.front() = driver.string();
    const std::string identity_flag = target.is_darwin()
        ? "-Wl,-install_name,@rpath/" + library.filename_string()
        : "-Wl,-soname," + library.filename_string();
    check(std::ranges::find(link_library.command, identity_flag) !=
              link_library.command.end(),
          "dylib link intent omitted its canonical identity");
    auto explicit_link = library_link;
    const std::string explicit_identity = target.is_darwin()
        ? "-Wl,-install_name,@loader_path/custom-probe.dylib"
        : "-Wl,-soname,libprobe-explicit.so";
    explicit_link.extra_flags = {explicit_identity};
    auto explicit_command = bake::make_link_command(target, explicit_link);
    check(std::ranges::find(explicit_command, explicit_identity) !=
              explicit_command.end() &&
              std::ranges::find(explicit_command, identity_flag) ==
                  explicit_command.end(),
          "dylib link intent replaced an explicit library identity");

    bake::BuildAction compile_main;
    compile_main.type = bake::BuildAction::Type::Compile;
    compile_main.id = "compile:dylib-main";
    compile_main.moid_id = "application";
    compile_main.moid = "dylib-probe";
    compile_main.inputs = {main_source};
    compile_main.outputs = {main_object};
    compile_main.command = {driver.string(), "c++", "-c", "-std=c++20",
                            main_source.string(), "-o", main_object.string()};

    bake::BuildAction link_main;
    link_main.type = bake::BuildAction::Type::Link;
    link_main.id = "link:dylib-main";
    link_main.moid_id = "application";
    link_main.moid = "dylib-probe";
    link_main.inputs = {main_object, library};
    link_main.outputs = {executable};
    link_main.depends_on = {1, 2};
    bake::LinkCommand executable_link;
    executable_link.objects = {main_object};
    executable_link.libraries = {library};
    executable_link.output = executable;
    link_main.command = bake::make_link_command(target, executable_link);
    link_main.command.front() = driver.string();

    bake::BuildGraph graph;
    graph.project_root = fixture.root;
    graph.state_dir = paths->bake;
    graph.actions = {std::move(compile_library), std::move(link_library),
                     std::move(compile_main), std::move(link_main)};
    auto snapshot = std::make_shared<bake::ProjectSnapshot>();
    snapshot->context = *context;
    snapshot->declaration_inputs = {*context_identity, {}};
    snapshot->build_graph = std::move(graph);
    fixture.handle.context = *context;
    fixture.handle.paths = *paths;
    fixture.handle.snapshot_id = {
        bake::SHA256::hex("core-build-dylib-snapshot")};
    fixture.handle.snapshot = std::move(snapshot);

    auto& loop = kota::event_loop::current();
    bake::CoreRuntime runtime(loop, {.max_concurrency = 2,
                                     .background_concurrency = 2});
    bake::CoreBuildService builds(runtime);
    auto operation = builds.start(fixture.handle,
                                  {.request = {"dylib-relocation"}});
    check(operation.has_value(), "dylib build did not start");
    auto result = co_await operation->wait();
    check(bake::status_of(result.outcome) == bake::ResultStatus::ready,
          "dylib build did not publish ready");
    check(!std::filesystem::exists(
              (paths->work / result.request.value).fs()),
          "successful dylib build retained request staging");
    const auto& artifacts = std::get<bake::ReadyBuild>(result.outcome).artifacts;
    auto executable_artifact = std::ranges::find_if(
        artifacts, [&](const bake::ArtifactIdentity& artifact) {
            return artifact.path == executable;
        });
    check(executable_artifact != artifacts.end(),
          "ready dylib build omitted its executable");
    auto library_content = bake::read_file(library);
    check(library_content.has_value(), "ready dylib build omitted its library");
    bake::write_file(library, "dependent-corruption\n");
    check(!builds.capture_execution(
              fixture.handle, result, *executable_artifact),
          "dependent dylib corruption passed execution capture");
    bake::write_file(library, *library_content);
    bake::Path capture_root;
    {
        auto capture = builds.capture_execution(
            fixture.handle, result, *executable_artifact);
        check(capture.has_value(), "stable execution capture failed");
        capture_root = capture->executable().parent().parent();
        bake::write_file(executable, "replaced after ready\n");
        std::error_code remove_error;
        std::filesystem::remove(library.fs(), remove_error);
        check(!remove_error, "canonical dylib could not be removed");
        auto run = bake::run_process(
            {capture->executable().string()}, fixture.root);
        check(run.success(),
              "captured executable did not use its captured dylib closure");
    }
    check(!std::filesystem::exists(capture_root.fs()),
          "execution capture was not cleaned after its lifetime");
    co_await runtime.shutdown();
    co_return true;
}

}

int main(int argc, char** argv) {
    executable_path = std::filesystem::absolute(argv[0]).string();
    bake_path = discover_bake();
    if (argc >= 4 && std::string_view(argv[1]) == "--child")
        return child_main(argc, argv);

    std::string selected = "all";
    for (int i = 1; i + 1 < argc; ++i)
        if (std::string_view(argv[i]) == "--case") selected = argv[i + 1];
    try {
        int cases = 0;
        auto run = [&](auto factory) {
            auto [result] = kota::run(factory());
            check(result.has_value() && *result, "scenario returned false");
            ++cases;
        };
        if (selected == "all" || selected == "staged-success")
            run(staged_success_case);
        if (selected == "all" || selected == "stale-kill-race")
            run(stale_kill_race_case);
        if (selected == "all" || selected == "publication-atomicity")
            run(publication_atomicity_case);
        if (selected == "all" || selected == "independent-insertion")
            run(independent_insertion_case);
        if (selected == "all" || selected == "selected-capture")
            run(selected_capture_case);
        if (selected == "all" || selected == "module-relocation")
            run(module_relocation_case);
        if (selected == "all" || selected == "dylib-relocation")
            run(dylib_relocation_case);
        check(cases != 0, "unknown or empty case selection");
        std::println("PASS core_build_test case={} cases={} checks={}",
                     selected, cases, checks);
        return 0;
    } catch (const std::exception& error) {
        std::println(std::cerr, "FAIL core_build_test case={}: {}",
                     selected, error.what());
        return 1;
    }
}
