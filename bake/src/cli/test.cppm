module;

#include <kota/async/io/loop.h>
#include <kota/async/runtime/task.h>

export module bake.cli.test;

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

struct Registration {
  std::string owner;
  std::string name;
  std::string binary;
  bool is_default = false;
};

std::vector<Registration> test_registrations(const BuiltProject &built) {
  std::vector<Registration> registrations;
  for (const auto &root : built.project.snapshot->moid_graph.roots) {
    auto node = built.project.snapshot->moid_graph.nodes.find(root);
    if (node == built.project.snapshot->moid_graph.nodes.end())
      continue;
    for (const auto &test : node->second.declaration.tests)
      registrations.push_back(
          {binary_moid_id(root.value, test.binary), test.name, test.binary,
           test.is_default});
  }
  return registrations;
}

const ArtifactIdentity *test_artifact(const BuiltProject &built,
                                      const Registration &registration) {
  const auto *ready = std::get_if<ReadyBuild>(&built.result.outcome);
  if (!ready)
    return nullptr;
  const std::string output_name = library_name(
      registration.binary, MoidType::Executable,
      built.project.context.target);
  const ArtifactIdentity *selected = nullptr;
  for (const auto &artifact : ready->artifacts) {
    if (artifact.owner_moid != registration.owner ||
        artifact.output_name != output_name)
      continue;
    if (selected)
      return nullptr;
    selected = &artifact;
  }
  return selected;
}

kota::task<int> test_command(const ParsedArgs &args) {
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

  auto registrations = test_registrations(*built);
  if (registrations.empty()) {
    co_await runtime.shutdown();
    std::println("bake: no tests registered");
    co_return 0;
  }

  std::vector<Registration> selected;
  if (args.has_option("all")) {
    selected = registrations;
  } else if (!args.positional.empty()) {
    for (const auto &registration : registrations)
      if (registration.name == args.positional.front())
        selected.push_back(registration);
    if (selected.empty()) {
      co_await runtime.shutdown();
      std::println(std::cerr, "bake: no test named '{}'",
                   args.positional.front());
      co_return 1;
    }
  } else {
    const Registration *default_test = nullptr;
    for (const auto &registration : registrations)
      if (registration.is_default)
        default_test = &registration;
    if (!default_test) {
      co_await runtime.shutdown();
      std::println(std::cerr,
                   "bake: no default test registered; use 'bake test <name>' "
                   "or --all");
      co_return 1;
    }
    selected.push_back(*default_test);
  }

  int failures = 0;
  CoreBuildService service(runtime);
  for (const auto &registration : selected) {
    const ArtifactIdentity *artifact = test_artifact(*built, registration);
    if (!artifact) {
      std::println(std::cerr,
                   "bake: test binary '{}' is missing from this build result",
                   registration.binary);
      ++failures;
      continue;
    }
    auto capture = service.capture_execution(
        built->project, built->result, *artifact);
    if (!capture) {
      std::println(std::cerr, "bake: cannot prepare test binary '{}': {}",
                   registration.binary, capture.error().message);
      ++failures;
      continue;
    }
    std::println("   Running {}", registration.name);
    auto result = run_process(capture->executable().string(), {},
                              built->project.context.workspace_root);
    if (result.success())
      std::println("   Passed {}", registration.name);
    else {
      std::println(std::cerr, "   Failed {} (exit {})", registration.name,
                   result.exit_code);
      ++failures;
    }
  }

  if (failures > 0)
    std::println(std::cerr, "bake: {} of {} selected test(s) failed", failures,
                 selected.size());
  co_await runtime.shutdown();
  co_return failures > 0 ? 1 : 0;
}

} // namespace

export int cmd_test(const ParsedArgs &args) {
  auto [result] = kota::run(test_command(args));
  if (!result) {
    std::println(std::cerr, "bake: test operation did not complete");
    return 1;
  }
  return *result;
}

} // namespace bake::cli
