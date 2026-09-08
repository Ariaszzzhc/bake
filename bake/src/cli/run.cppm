module;

#include <kota/async/io/loop.h>
#include <kota/async/runtime/task.h>

export module bake.cli.run;

import std;
import bake.util;
import bake.buildsystem.cmdgen;
import bake.buildsystem.graph;
import bake.buildsystem.moid;
import bake.buildsystem.project;
import bake.core.build;
import bake.core.model;
import bake.core.runtime;
import bake.core.snapshot;
import bake.cli;
import bake.cli.build;

namespace bake::cli {
namespace {

const ArtifactIdentity *main_executable(const BuiltProject &built,
                                        std::string &error) {
  const auto *ready = std::get_if<ReadyBuild>(&built.result.outcome);
  if (!ready) {
    error = "build result is not ready";
    return nullptr;
  }

  struct Candidate {
    std::string owner;
    std::string name;
  };
  std::vector<Candidate> candidates;
  for (const auto &root : built.project.snapshot->moid_graph.roots) {
    auto node = built.project.snapshot->moid_graph.nodes.find(root);
    if (node == built.project.snapshot->moid_graph.nodes.end() ||
        node->second.declaration.type != MoidType::Executable)
      continue;
    candidates.push_back({root.value, node->second.declaration.name});
  }
  if (candidates.empty()) {
    error = "cannot run non-executable package";
    return nullptr;
  }
  if (candidates.size() > 1) {
    std::ranges::sort(candidates, {}, &Candidate::owner);
    std::string choices;
    for (const auto &candidate : candidates) {
      if (!choices.empty())
        choices += ", ";
      choices += candidate.owner + " (" + candidate.name + ")";
    }
    error = "multiple executable packages selected; use -p <member> to "
            "choose one: " + choices;
    return nullptr;
  }

  const auto &candidate = candidates.front();
  const std::string output_name = library_name(
      candidate.name, MoidType::Executable, built.project.context.target);
  const ArtifactIdentity *selected = nullptr;
  for (const auto &artifact : ready->artifacts) {
    if (artifact.owner_moid != candidate.owner ||
        artifact.output_name != output_name)
      continue;
    if (selected) {
      error = "build result contains duplicate executable artifacts";
      return nullptr;
    }
    selected = &artifact;
  }
  if (!selected)
    error = "build result does not contain the selected executable";
  return selected;
}

kota::task<int> run_command(const ParsedArgs &args) {
  auto &loop = kota::event_loop::current();
  const auto concurrency = requested_concurrency(args);
  CoreRuntime runtime(
      loop, {.max_concurrency = concurrency,
             .background_concurrency = concurrency > 1 ? concurrency - 1 : 1});
  auto built = co_await build_project(runtime, args);
  if (!built) {
    co_await runtime.shutdown();
    std::println(std::cerr, "bake: {}", built.error());
    co_return 1;
  }

  std::string error;
  const ArtifactIdentity *executable = main_executable(*built, error);
  if (!executable) {
    co_await runtime.shutdown();
    std::println(std::cerr, "bake: {}", error);
    co_return 1;
  }

  CoreBuildService service(runtime);
  auto capture = service.capture_execution(
      built->project, built->result, *executable);
  if (!capture) {
    co_await runtime.shutdown();
    std::println(std::cerr, "bake: {}", capture.error().message);
    co_return 1;
  }

  auto result = run_process(capture->executable().string(), args.positional,
                            built->project.context.workspace_root);
  co_await runtime.shutdown();
  co_return result.exit_code;
}

} // namespace

export int cmd_run(const ParsedArgs &args) {
  auto [result] = kota::run(run_command(args));
  if (!result) {
    std::println(std::cerr, "bake: run operation did not complete");
    return 1;
  }
  return *result;
}

} // namespace bake::cli
