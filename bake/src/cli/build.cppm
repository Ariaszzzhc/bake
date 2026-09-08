module;

#include <kota/async/io/loop.h>
#include <kota/async/runtime/task.h>

export module bake.cli.build;

import std;
import bake.util;
import bake.buildsystem.graph;
import bake.buildsystem.plan;
import bake.core.build;
import bake.core.model;
import bake.core.project;
import bake.core.runtime;
import bake.core.snapshot;
import bake.cli;
import bake.cli.compdb;
import bake.cli.project;

namespace bake::cli {

export struct BuiltProject {
  ProjectHandle project;
  BuildResult result;
};

export kota::task<std::expected<BuiltProject, std::string>>
build_project(CoreRuntime &runtime, const ParsedArgs &args);

namespace {

std::string project_label(const ProjectHandle &project) {
  const auto &graph = project.snapshot->moid_graph;
  if (graph.roots.size() == 1) {
    auto node = graph.nodes.find(graph.roots.front());
    if (node != graph.nodes.end())
      return node->second.declaration.name + " v" +
             node->second.declaration.version;
  }
  std::string label = project.context.workspace_root.filename_string();
  return label.empty() ? project.context.workspace_root.string() : label;
}

std::string build_failure(const BuildOutcome &outcome) {
  return std::visit([](const auto &value) -> std::string {
    using T = std::remove_cvref_t<decltype(value)>;
    if constexpr (std::is_same_v<T, ReadyBuild>) {
      return {};
    } else if constexpr (std::is_same_v<T, FailedBuild>) {
      return value.message;
    } else if constexpr (std::is_same_v<T, CancelledBuild>) {
      return value.reason;
    } else {
      return value.message;
    }
  }, outcome);
}

kota::task<int> build_command(const ParsedArgs &args) {
  auto &loop = kota::event_loop::current();
  const auto concurrency = requested_concurrency(args);
  CoreRuntime runtime(
      loop, {.max_concurrency = concurrency,
             .background_concurrency = concurrency > 1 ? concurrency - 1 : 1});
  auto built = co_await build_project(runtime, args);
  co_await runtime.shutdown();
  if (!built) {
    std::println(std::cerr, "bake: {}", built.error());
    co_return 1;
  }
  co_return 0;
}

} // namespace

kota::task<std::expected<BuiltProject, std::string>>
build_project(CoreRuntime &runtime, const ParsedArgs &args) {
  auto project = co_await open_project(runtime, args);
  if (!project)
    co_return std::unexpected(project.error());

  std::println("   Building {}", project_label(*project));
  auto graph_export = write_graph_json(
      project->snapshot->build_graph, project->paths.bake / "graph.json");
  if (!graph_export)
    co_return std::unexpected(graph_export.error());
  BuildOptions options;
  options.lane = RuntimeLane::foreground;
  options.verbose = args.has_option("v") || args.has_option("verbose");
  const bool verbose = options.verbose;
  options.progress = [verbose](const BuildProgress &progress) {
    if (progress.kind == BuildProgressKind::moid_started) {
      if (progress.version.empty())
        std::println("   Compiling {}", progress.moid);
      else
        std::println("   Compiling {} v{}", progress.moid, progress.version);
      return;
    }
    if (!verbose)
      return;
    const std::string_view state =
        progress.kind == BuildProgressKind::action_reused ? "cached" : "build";
    std::println("    [{}] {}", state, progress.description);
  };
  const auto started = std::chrono::steady_clock::now();
  CoreBuildService service(runtime);
  auto operation = service.start(*project, std::move(options));
  if (!operation)
    co_return std::unexpected(operation.error().message);
  BuildResult result = co_await operation->wait();
  if (status_of(result.outcome) != ResultStatus::ready)
    co_return std::unexpected(build_failure(result.outcome));
  auto valid = service.validate_ready(*project, result);
  if (!valid)
    co_return std::unexpected(valid.error().message);
  const auto elapsed = std::chrono::duration<double>(
      std::chrono::steady_clock::now() - started).count();
  std::println("    Finished in {:.2f}s", elapsed);

  auto exported = write_compilation_database(
      *project, project->context.workspace_root / "compile_commands.json");
  if (!exported)
    co_return std::unexpected(exported.error());
  co_return BuiltProject{std::move(*project), std::move(result)};
}

export int cmd_build(const ParsedArgs &args) {
  auto [result] = kota::run(build_command(args));
  if (!result) {
    std::println(std::cerr, "bake: build operation did not complete");
    return 1;
  }
  return *result;
}

} // namespace bake::cli
