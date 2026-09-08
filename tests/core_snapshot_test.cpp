#include <stdlib.h>

import std;
import bake.json;
import bake.util;
import bake.toolchain.target;
import bake.buildsystem.project;
import bake.buildsystem.moid;
import bake.buildsystem.graph;
import bake.buildsystem.plan;
import bake.core.model;
import bake.core.snapshot;

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, std::string_view message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::println(std::cerr, "FAIL: {}", message);
    }
}

struct Fixture {
    bake::Path root;
    bake::Path manifest;
    bake::Path lock;
    bake::Path build_script;
    bake::ProjectContext context;
    bake::ProjectContextKey key;

    Fixture() {
        root = bake::Path(std::filesystem::temp_directory_path() /
            ("bake-core-snapshot-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count())));
        root.mkdir_recursive();
        root = bake::Path(std::filesystem::canonical(root.fs()));
        manifest = root / "bake.toml";
        lock = root / "bake.lock";
        build_script = root / "build.cpp";
        bake::write_file(manifest, "[package]\nname='app'\n");
        bake::write_file(lock, "{\"deps\":{}}\n");
        bake::write_file(build_script, "int main() { return 0; }\n");
        bake::Path compiler = root / "bake";
        bake::write_file(compiler, "compiler");
        auto identity = bake::identify_compiler(
            compiler, "snapshot-test", root / "identity-cache");
        if (!identity) throw std::runtime_error(identity.error().message);
        auto made = bake::make_project_context(
            root, root, std::vector<std::string>{"async"}, "dev",
            bake::parse_target("x86_64-linux-gnu.2.36"), *identity);
        if (!made) throw std::runtime_error(made.error().message);
        context = *made;
        auto context_identity = bake::context_key(context);
        if (!context_identity)
            throw std::runtime_error(context_identity.error().message);
        key = *context_identity;
    }

    ~Fixture() {
        std::error_code ignored;
        std::filesystem::remove_all(root.fs(), ignored);
    }

    bake::InputDeclarationRecord declaration_record() const {
        auto observed = bake::observe_declaration_inputs(
            key, {manifest}, lock, {build_script});
        if (!observed) throw std::runtime_error(observed.error().message);
        return *observed;
    }

    bake::InputIdentity identity(const bake::Path& path) const {
        auto content = bake::read_file(path);
        if (!content) throw std::runtime_error("missing fixture input");
        return bake::InputIdentity{
            path, bake::SHA256::hex(*content), content->size(),
        };
    }
};

bake::MoidDeclaration declaration(const Fixture& fixture) {
    bake::MoidDeclaration value;
    value.id = "app";
    value.name = "app";
    value.version = "1.2.3";
    value.root = fixture.root.string();
    value.cxx_std = "c++23";
    value.c_std = "c17";
    value.active_features = {"async"};
    value.sources = {
        {fixture.root.string() + "/src/api.cppm", true},
        {fixture.root.string() + "/src/main.cpp", false},
    };
    value.public_include_dirs = {fixture.root.string() + "/include"};
    value.libraries = {"z"};
    value.frameworks = {};
    value.compile_flags = {"-Wall"};
    value.compile_defines = {{"APP", "1"}};
    value.public_compile_defines = {{"APP_API", "1"}};
    value.extra_include_dirs = {fixture.root.string() + "/generated"};
    value.link_flags = {"-pthread"};
    value.prebuilt_libs = {fixture.root.string() + "/libsupport.a"};
    value.binaries = {{"tool", {{fixture.root.string() + "/tool.cpp", false}}, {}}};
    value.tests = {{"tool-test", "tool", true}};
    value.used_source_deps = {"vendor"};
    return value;
}

bake::ProjectSnapshot make_snapshot(Fixture& fixture) {
    bake::Path source_dir = fixture.root / "src";
    source_dir.mkdir_recursive();
    bake::Path module = source_dir / "api.cppm";
    bake::Path source = source_dir / "main.cpp";
    bake::Path header = fixture.root / "include" / "api.hpp";
    header.parent().mkdir_recursive();
    bake::write_file(module, "export module api; export int answer();\n");
    bake::write_file(source, "import api; int main() { return answer(); }\n");
    bake::write_file(header, "#pragma once\n");

    bake::MoidGraph moid_graph;
    bake::MoidNode node;
    node.id = bake::MoidId{"app"};
    node.declaration = declaration(fixture);
    node.source_deps["vendor"] = fixture.root.string() + "/vendor";
    moid_graph.nodes.emplace(node.id, node);
    moid_graph.roots.push_back(node.id);

    bake::Path state_dir = fixture.root / "out" /
        "x86_64-linux-gnu.2.36" / "dev" / ".bake";
    bake::Path bmi = state_dir.parent() / ".bmi" / "api.pcm";
    bake::Path module_object = state_dir.parent() / ".obj" / "api.o";
    bake::Path object = state_dir.parent() / ".obj" / "main.o";
    bake::BuildAction module_action{
        bake::BuildAction::Type::CompileModule, "module:app:api", "app",
        "api.cppm", "app", "1.2.3", {module}, {module_object, bmi},
        {fixture.context.compiler.executable.string(), "c++", "-c",
         module.string(), "-o", module_object.string()}, {},
    };
    bake::BuildAction compile_action{
        bake::BuildAction::Type::Compile, "compile:app:main", "app",
        "main.cpp", "app", "1.2.3", {source, header, bmi}, {object},
        {fixture.context.compiler.executable.string(), "c++", "-c",
         source.string(), "-o", object.string()}, {0},
    };
    bake::BuildGraph build_graph{{module_action, compile_action}, state_dir,
                                 fixture.root};

    bake::CompilationDescription module_compilation{
        {module_action.id}, fixture.key, "app",
        bake::SourceLanguage::module_interface, fixture.root, module,
        module_object,
        {"c++", "-c", module.string(), "-o", module_object.string()}, {},
    };
    bake::CompilationDescription compilation{
        {compile_action.id}, fixture.key, "app", bake::SourceLanguage::cxx,
        fixture.root, source, object,
        {"c++", "-c", source.string(), "-o", object.string()},
        {{"api", bmi}},
    };

    bake::SourceIndex sources;
    sources.entries.push_back(bake::LockedSource{
        "https://example.com/vendor", "v1", "tag", "0123456789abcdef",
        "sha256-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        fixture.root / "vendor",
    });

    return bake::ProjectSnapshot{
        fixture.context,
        fixture.declaration_record(),
        std::move(sources),
        {{fixture.manifest, std::string("app"), {"."},
          bake::SourceExtConfig{}}},
        std::move(moid_graph),
        {{
            {{module_action.id}, "app", fixture.identity(module), {},
             std::string("api"), {}},
            {{compile_action.id}, "app", fixture.identity(source),
             {fixture.identity(header)}, std::nullopt, {"api"}},
        }},
        std::move(build_graph),
        {std::move(module_compilation), std::move(compilation)},
    };
}

void round_trip_case() {
    Fixture fixture;
    auto snapshot = make_snapshot(fixture);
    const std::string encoded = bake::encode_project_snapshot(snapshot);
    auto restored = bake::decode_project_snapshot(encoded, fixture.key);
    check(restored.has_value(), "complete snapshot decodes");
    if (!restored) {
        std::println(std::cerr, "decode error {}: {}", restored.error().field,
                     restored.error().message);
        return;
    }
    check(bake::encode_project_snapshot(*restored) == encoded,
          "canonical snapshot bytes survive cold restore");
    check(restored->source_index.entries.size() == 1 &&
              restored->source_index.entries.front().commit ==
                  "0123456789abcdef",
          "locked source identity survives restore");
    check(restored->moid_graph.nodes.at(bake::MoidId{"app"})
                  .declaration.used_source_deps ==
              std::vector<std::string>{"vendor"},
          "complete unified declaration survives restore");
    check(restored->build_graph.actions.size() == 2 &&
              restored->build_graph.actions[1].depends_on ==
                  std::vector<std::size_t>{0},
          "lowered action dependency survives restore");
    check(restored->compilations.size() == 2 &&
              restored->compilations[1].module_references.front().name == "api",
          "compilation description and module reference survive restore");
    check(restored->source_graph.units[1].headers.front().digest ==
              fixture.identity(fixture.root / "include" / "api.hpp").digest,
          "strong header identity survives restore");

    auto graph_json = bake::build_graph_to_json(snapshot.build_graph);
    auto graph = bake::build_graph_from_json(graph_json);
    check(graph && bake::build_graph_to_json(*graph).dump() == graph_json.dump(),
          "public graph JSON uses the same faithful strict codec");

    bake::Path graph_path = fixture.root / "graph.json";
    auto written = bake::write_graph_json(snapshot.build_graph, graph_path);
    auto reloaded = bake::read_graph_json(graph_path);
    check(written && reloaded &&
              bake::build_graph_to_json(*reloaded).dump() == graph_json.dump(),
          "external graph projection atomically reloads through the strict codec");
    bool temporary_left = false;
    for (const auto& entry :
         std::filesystem::directory_iterator(fixture.root.fs())) {
        if (entry.path().filename().string().contains(".tmp"))
            temporary_left = true;
    }
    check(!temporary_left, "successful graph projection leaves no temp file");
}

void corrupt_required_fields_case() {
    Fixture fixture;
    auto snapshot = make_snapshot(fixture);
    auto valid = bake::project_snapshot_to_json(snapshot);

    bake::json::Value unsupported = valid;
    unsupported["schema"] = "bake-core-project-snapshot-v999";
    auto unsupported_result =
        bake::project_snapshot_from_json(unsupported, fixture.key);
    check(!unsupported_result && unsupported_result.error().code ==
              bake::SnapshotErrorCode::unsupported_schema,
          "unsupported schema is distinct from corruption");

    for (std::string field : {"context", "declaration_inputs", "source_index",
                              "manifests", "moid_graph", "source_graph",
                              "build_graph", "compilations"}) {
        auto wrong_type = valid;
        wrong_type[field] = bake::json::Value(nullptr);
        auto result = bake::project_snapshot_from_json(wrong_type, fixture.key);
        check(!result && result.error().code !=
                             bake::SnapshotErrorCode::unsupported_schema,
              "wrong-typed required field is rejected: " + field);
    }

    auto missing_graph = valid;
    missing_graph["build_graph"] = bake::json::Value::object();
    auto missing_graph_result =
        bake::project_snapshot_from_json(missing_graph, fixture.key);
    check(!missing_graph_result && missing_graph_result.error().code ==
              bake::SnapshotErrorCode::malformed,
          "missing graph fields cannot become empty success");

    auto wrong_context = valid;
    wrong_context["declaration_inputs"]["context"] = bake::SHA256::hex("wrong");
    auto wrong_context_result =
        bake::project_snapshot_from_json(wrong_context, fixture.key);
    check(!wrong_context_result && wrong_context_result.error().code ==
              bake::SnapshotErrorCode::context_mismatch,
          "declaration record from another context is rejected");

    auto wrong_owner = valid;
    wrong_owner["build_graph"]["actions"][std::size_t{0}]["moid_id"] = "ghost";
    auto wrong_owner_result =
        bake::project_snapshot_from_json(wrong_owner, fixture.key);
    check(!wrong_owner_result && wrong_owner_result.error().code ==
              bake::SnapshotErrorCode::unknown_owner,
          "build action with unknown owner is rejected");

    auto unknown_dependency = valid;
    unknown_dependency["build_graph"]["actions"][std::size_t{1}]["depends_on"] =
        std::vector<std::string>{"ghost-action"};
    auto unknown_dependency_result =
        bake::project_snapshot_from_json(unknown_dependency, fixture.key);
    check(!unknown_dependency_result && unknown_dependency_result.error().code ==
              bake::SnapshotErrorCode::invalid_dependency,
          "unknown action dependency is rejected");

    auto wrong_declaration = valid;
    wrong_declaration["moid_graph"]["nodes"][std::size_t{0}]
                     ["declaration"]["id"] = 7;
    auto wrong_declaration_result =
        bake::project_snapshot_from_json(wrong_declaration, fixture.key);
    check(!wrong_declaration_result && wrong_declaration_result.error().code ==
              bake::SnapshotErrorCode::malformed,
          "wrong-typed declaration identity is rejected");

    auto truncated = bake::decode_project_snapshot("{\"schema\":", fixture.key);
    check(!truncated && truncated.error().code ==
              bake::SnapshotErrorCode::malformed,
          "truncated snapshot JSON is rejected");
}

void declaration_mtime_matrix_case() {
    Fixture fixture;
    auto baseline = fixture.declaration_record();
    auto observe = [&] { return fixture.declaration_record(); };
    const auto manifest_time = std::filesystem::last_write_time(fixture.manifest.fs());
    const auto lock_time = std::filesystem::last_write_time(fixture.lock.fs());
    const auto script_time = std::filesystem::last_write_time(fixture.build_script.fs());
    for (const auto& [path, original, name] :
         std::vector<std::tuple<bake::Path, std::filesystem::file_time_type,
                                std::string>>{
             {fixture.manifest, manifest_time, "manifest"},
             {fixture.lock, lock_time, "lock"},
             {fixture.build_script, script_time, "build script"},
         }) {
        std::filesystem::last_write_time(path.fs(), original + std::chrono::seconds(5));
        check(!bake::declaration_inputs_match(baseline, observe()),
              name + " newer mtime invalidates");
        std::filesystem::last_write_time(path.fs(), original);
        std::filesystem::last_write_time(path.fs(), original - std::chrono::seconds(5));
        check(!bake::declaration_inputs_match(baseline, observe()),
              name + " older mtime invalidates");
        std::filesystem::last_write_time(path.fs(), original);
        std::filesystem::last_write_time(path.fs(), original + std::chrono::seconds(1));
        check(!bake::declaration_inputs_match(baseline, observe()),
              name + " same-content touch invalidates");
        std::filesystem::last_write_time(path.fs(), original);
        std::filesystem::remove(path.fs());
        check(!bake::declaration_inputs_match(baseline, observe()),
              name + " deletion invalidates");
        if (name == "manifest") bake::write_file(path, "[package]\nname='app'\n");
        else if (name == "lock") bake::write_file(path, "{\"deps\":{}}\n");
        else bake::write_file(path, "int main() { return 0; }\n");
        std::filesystem::last_write_time(path.fs(), original);
    }

    bake::Path optional_script = fixture.root / "missing-build.cpp";
    auto missing = bake::observe_declaration_inputs(
        fixture.key, {fixture.manifest}, fixture.lock, {optional_script});
    bake::write_file(optional_script, "int main(){}\n");
    auto created = bake::observe_declaration_inputs(
        fixture.key, {fixture.manifest}, fixture.lock, {optional_script});
    check(missing && created && !bake::declaration_inputs_match(*missing, *created),
          "optional build script creation invalidates");

    bake::Path missing_manifest = fixture.root / "member" / "bake.toml";
    missing_manifest.parent().mkdir_recursive();
    auto before_manifest_create = bake::observe_declaration_inputs(
        fixture.key, {missing_manifest}, fixture.lock, {});
    bake::write_file(missing_manifest, "[package]\nname='member'\n");
    auto after_manifest_create = bake::observe_declaration_inputs(
        fixture.key, {missing_manifest}, fixture.lock, {});
    check(before_manifest_create && after_manifest_create &&
              !bake::declaration_inputs_match(*before_manifest_create,
                                              *after_manifest_create),
          "manifest creation invalidates");

    bake::Path missing_lock = fixture.root / "other.lock";
    auto before_lock_create = bake::observe_declaration_inputs(
        fixture.key, {fixture.manifest}, missing_lock, {});
    bake::write_file(missing_lock, "{\"deps\":{}}\n");
    auto after_lock_create = bake::observe_declaration_inputs(
        fixture.key, {fixture.manifest}, missing_lock, {});
    check(before_lock_create && after_lock_create &&
              !bake::declaration_inputs_match(*before_lock_create,
                                              *after_lock_create),
          "lock creation invalidates");

    bake::Path extra = fixture.root / "extra.txt";
    bake::write_file(extra, "ignored");
    check(bake::declaration_inputs_match(baseline, observe()),
          "extra file and directory state do not invalidate declarations");
    setenv("BAKE_SNAPSHOT_TEST_ENV", "changed", 1);
    check(bake::declaration_inputs_match(baseline, observe()),
          "environment changes do not invalidate declarations");
    unsetenv("BAKE_SNAPSHOT_TEST_ENV");

    for (const auto& [path, original, name] :
         std::vector<std::tuple<bake::Path, std::filesystem::file_time_type,
                                std::string>>{
             {fixture.manifest, manifest_time, "manifest"},
             {fixture.lock, lock_time, "lock"},
             {fixture.build_script, script_time, "build script"},
         }) {
        auto original_content = bake::read_file(path).value();
        bake::write_file(path, std::string(original_content.size(), '#'));
        std::filesystem::last_write_time(path.fs(), original);
        check(bake::declaration_inputs_match(baseline, observe()),
              name + " content change with restored mtime stays reusable");
        bake::write_file(path, original_content);
        std::filesystem::last_write_time(path.fs(), original);
    }
}

}

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--case" &&
            std::string_view(argv[2]) == "declaration-mtime-matrix") {
            declaration_mtime_matrix_case();
        } else if (argc == 3 && std::string_view(argv[1]) == "--case" &&
                   std::string_view(argv[2]) == "corrupt-required-fields") {
            corrupt_required_fields_case();
        } else {
            round_trip_case();
            declaration_mtime_matrix_case();
            corrupt_required_fields_case();
        }
    } catch (const std::exception& exception) {
        std::println(std::cerr, "FAIL: unexpected exception: {}", exception.what());
        ++failures;
    }
    std::println("core_snapshot_test: {} checks, {} failures", checks, failures);
    return failures == 0 ? 0 : 1;
}
