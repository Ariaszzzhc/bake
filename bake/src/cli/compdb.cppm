module;

#include <kota/async/io/loop.h>
#include <kota/async/runtime/task.h>

export module bake.cli.compdb;

import std;
import bake.json;
import bake.util;
import bake.core.model;
import bake.core.project;
import bake.core.runtime;
import bake.core.snapshot;
import bake.cli;
import bake.cli.project;

namespace bake::cli {

export std::expected<std::size_t, std::string>
write_compilation_database(const ProjectHandle &project, const Path &output) {
  json::Value commands = json::Value::array();
  for (const auto &description : project.snapshot->compilations) {
    json::Value item;
    item["directory"] =
        description.working_directory.absolute().fs().generic_string();
    item["file"] = description.source.absolute().fs().generic_string();
    if (!description.planned_output.string().empty())
      item["output"] =
          description.planned_output.absolute().fs().generic_string();
    item["arguments"] = json::Value::array();
    for (const auto &argument : materialize_compiler_command(
             description, project.context.compiler))
      item["arguments"].push_back(argument);
    commands.push_back(std::move(item));
  }
  if (!atomic_write_file(output, commands.dump(2) + "\n"))
    return std::unexpected("failed to write " + output.string());
  return project.snapshot->compilations.size();
}

namespace {

kota::task<int> compdb_command(const ParsedArgs &args) {
  auto &loop = kota::event_loop::current();
  CoreRuntime runtime(loop, {.max_concurrency = 1,
                             .background_concurrency = 1});
  auto project = co_await open_project(runtime, args);
  if (!project) {
    std::println(std::cerr, "bake: {}", project.error());
    co_await runtime.shutdown();
    co_return 1;
  }

  Path output = project->context.workspace_root / "compile_commands.json";
  if (!args.positional.empty())
    output = Path(args.positional.front()).absolute();
  auto written = write_compilation_database(*project, output);
  co_await runtime.shutdown();
  if (!written) {
    std::println(std::cerr, "bake: {}", written.error());
    co_return 1;
  }
  std::println("Wrote {} ({} entries)", output.string(), *written);
  co_return 0;
}

} // namespace

export int cmd_compdb(const ParsedArgs &args) {
  auto [result] = kota::run(compdb_command(args));
  if (!result) {
    std::println(std::cerr, "bake: compdb operation did not complete");
    return 1;
  }
  return *result;
}

} // namespace bake::cli
