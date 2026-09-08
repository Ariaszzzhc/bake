#include <kota/async/io/loop.h>
#include <kota/async/runtime/task.h>

import std;
import bake.util;
import bake.core.model;
import bake.core.project;
import bake.core.runtime;
import bake.core.state;

namespace {

bake::Path compiler_executable;

void check(bool condition, std::string_view message) {
  if (!condition)
    throw std::runtime_error(std::string(message));
}

class Fixture {
public:
  Fixture() {
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    root = bake::Path(std::filesystem::temp_directory_path() /
                      ("bake-core-project-" + std::to_string(nonce)));
    (root / "src").mkdir_recursive();
    check(bake::write_file(
              root / "bake.toml",
              "[package]\nname = \"core-project-fixture\"\n"
              "version = \"0.1.0\"\n"),
          "failed to write fixture manifest");
    check(bake::write_file(root / "src" / "main.cpp",
                           "int main() { return 0; }\n"),
          "failed to write fixture source");
  }

  ~Fixture() {
    std::error_code error;
    std::filesystem::remove_all(root.fs(), error);
  }

  bake::Path root;
};

bake::ProjectRequest request_for(const Fixture &fixture) {
  bake::ProjectRequest request;
  request.root = fixture.root;
  request.profile = "dev";
  request.offline = true;
  request.compiler_executable = compiler_executable;
  request.compiler_version = "core-project-test";
  return request;
}

kota::task<bool> lifecycle_case() {
  Fixture fixture;
  auto &loop = kota::event_loop::current();
  bake::CoreRuntime runtime(
      loop, {.max_concurrency = 2, .background_concurrency = 1});
  bake::ProjectCore core(runtime);

  auto first = co_await core.open(request_for(fixture));
  check(first.has_value(), first ? "" : first.error().message);
  check(first->metrics.dependency_resolutions != 0,
        "initial open did not resolve the project");
  check(first->metrics.full_graph_lowerings == 1,
        "initial open did not lower one build graph");
  check(first->metrics.declaration_evaluations == 0,
        "default discovery unexpectedly evaluated build.cpp");
  const bake::Path first_snapshot =
      first->paths.snapshots / (first->snapshot_id.value + ".json");
  auto first_content = bake::read_file(first_snapshot);
  check(first_content.has_value(), "initial snapshot was not published");
  const auto first_mtime =
      std::filesystem::last_write_time(first_snapshot.fs());

  auto second = co_await core.open(request_for(fixture));
  check(second.has_value(), second ? "" : second.error().message);
  check(second->snapshot_id == first->snapshot_id,
        "unchanged project did not restore the same snapshot");
  check(second->metrics.dependency_resolutions == 0 &&
            second->metrics.declaration_preparations == 0 &&
            second->metrics.declaration_evaluations == 0 &&
            second->metrics.full_graph_lowerings == 0 &&
            second->metrics.source_updates == 0,
        "cold restore performed costly project work");
  check(bake::read_file(first_snapshot) == first_content &&
            std::filesystem::last_write_time(first_snapshot.fs()) ==
                first_mtime,
        "cold restore rewrote the immutable snapshot");

  check(bake::write_file(fixture.root / "compile_commands.json",
                         "corrupt external export"),
        "failed to create external compile database");
  std::filesystem::remove((fixture.root / "compile_commands.json").fs());
  auto without_compdb = co_await core.open(request_for(fixture));
  check(without_compdb.has_value() &&
            without_compdb->snapshot_id == first->snapshot_id,
        "compile_commands.json affected Core restore");

  check(bake::write_file(fixture.root / "src" / "main.cpp",
                         "int main() { return 7; }\n"),
        "failed to edit source body");
  auto body_changed = co_await core.open(request_for(fixture));
  check(body_changed.has_value(),
        body_changed ? "" : body_changed.error().message);
  check(body_changed->metrics.source_updates == 1 &&
            body_changed->metrics.declaration_preparations == 0 &&
            body_changed->metrics.declaration_evaluations == 0 &&
            body_changed->metrics.full_graph_lowerings == 0,
        "body-only edit performed declaration or lowering work");
  check(body_changed->snapshot_id != first->snapshot_id,
        "body-only edit retained the old source identity");

  check(bake::write_file(fixture.root / "src" / "extra.cpp",
                         "int extra() { return 1; }\n"),
        "failed to add default source");
  auto added = co_await core.open(request_for(fixture));
  check(added.has_value(), added ? "" : added.error().message);
  check(added->metrics.source_updates == 1 &&
            added->metrics.full_graph_lowerings == 1 &&
            added->metrics.declaration_evaluations == 0,
        "default source addition did not selectively update the graph");
  check(added->snapshot->compilations.size() ==
            body_changed->snapshot->compilations.size() + 1,
        "default source addition did not update compilation inventory");

  const auto stable = added->snapshot_id;
  check(bake::write_file(fixture.root / "bake.toml", "[package"),
        "failed to corrupt manifest");
  auto failed = co_await core.open(request_for(fixture));
  check(!failed && failed.error().state == bake::ProjectOpenState::failed,
        "failed refresh was reported as current");
  bake::ProjectStateStore store(added->paths, bake::context_key(
      added->context).value());
  auto latest = store.latest_successful_observation();
  check(latest && latest->snapshot == stable,
        "failed refresh replaced the prior successful snapshot");

  co_await runtime.shutdown();
  co_return true;
}

}

int main(int argc, char **argv) {
  try {
    compiler_executable =
        bake::Path(std::filesystem::canonical(argv[0]));
    std::string selected = argc > 2 && std::string_view(argv[1]) == "--case"
                               ? argv[2]
                               : "lifecycle";
    check(selected == "lifecycle" || selected == "cold-restore",
          "unknown project test case");
    auto [passed] = kota::run(lifecycle_case());
    check(passed && *passed, "project scenario returned false");
    std::println("PASS core_project_test case={}", selected);
    return 0;
  } catch (const std::exception &error) {
    std::println(std::cerr, "FAIL core_project_test: {}", error.what());
    return 1;
  }
}
