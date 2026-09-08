module;

#include <kota/async/runtime/task.h>

export module bake.cli.project;

import std;
import bake.util;
import bake.buildsystem.declaration;
import bake.core.project;
import bake.core.runtime;
import bake.cli;

namespace bake::cli {

export kota::task<std::expected<ProjectHandle, std::string>>
open_project(CoreRuntime &runtime, const ParsedArgs &args) {
  auto features = parse_requested_features(args);
  if (!features)
    co_return std::unexpected("invalid feature selection");

  std::string profile = args.has_option("release") ? "release" : "dev";
  if (auto selected = args.get_option("profile"))
    profile = *selected;

  std::optional<std::string> target;
  if (auto selected = args.get_option("target"))
    target = *selected;
  else if (auto selected = args.get_option("t"))
    target = *selected;

  ProjectRequest request;
  request.root = Path::current();
  request.member = args.get_option("p");
  request.features = std::move(*features);
  request.profile = std::move(profile);
  request.target_triple = std::move(target);
  request.locked = args.has_option("locked") || args.has_option("frozen");
  request.offline = args.has_option("offline") || args.has_option("frozen");
  request.compiler_executable = Path(bake_exe_path());
  request.compiler_version = bake_version();

  ProjectCore core(runtime);
  auto opened = co_await core.open(std::move(request));
  if (!opened)
    co_return std::unexpected(opened.error().message);
  if (!opened->declaration_stdout.empty())
    std::print("{}", opened->declaration_stdout);
  if (!opened->declaration_stderr.empty())
    std::print(std::cerr, "{}", opened->declaration_stderr);
  for (const auto &warning :
       declaration_warnings(opened->snapshot->moid_graph))
    std::println(std::cerr, "bake: warning: {}", warning);
  if (args.has_option("v") || args.has_option("verbose"))
    std::println(std::cerr, "   Core {}",
                 format_project_metrics(opened->metrics));
  co_return std::move(*opened);
}

} // namespace bake::cli
