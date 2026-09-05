// bake.cli.run — the `run` command.

export module bake.cli.run;

import std;
import bake.util;
import bake.toolchain.target;
import bake.toolchain.lld;
import bake.toolchain.driver;
import bake.toolchain.runtime;
import bake.buildsystem.project;
import bake.buildsystem.moid;
import bake.buildsystem.graph;
import bake.buildsystem.cmdgen;
import bake.buildsystem.engine;
import bake.buildsystem.package;
import bake.buildsystem.query;
import bake.json;
import bake.cli.build;
import bake.cli;

namespace bake::cli {

// ===== run command =====

export int cmd_run(const ParsedArgs &args) {
  // First build
  int build_result = cmd_build(args);
  if (build_result != 0)
    return build_result;

  // Find the built executable
  auto root = find_project_root();
  if (!root)
    return 1;

  Path exe_path;

  auto manifest = Manifest::load(*root);
  if (!manifest) {
    std::println(std::cerr, "bake: failed to load bake.toml");
    return 1;
  }

  auto selected_member =
      resolve_workspace_member_selection(*manifest, args.get_option("p"));
  if (!selected_member) {
    std::println(std::cerr, "bake: {}", selected_member.error());
    return 1;
  }

  BuildSelection selection;
  selection.workspace_member = std::move(*selected_member);
  ResolvePolicy policy;
  policy.offline = args.has_option("offline") || args.has_option("frozen");
  auto outer_graph = resolve_moid_graph(*manifest, selection, policy,
                          source_index_from_lock(*root));
  if (!outer_graph) {
    std::println(std::cerr, "bake: {}", outer_graph.error());
    return 1;
  }

  std::vector<MoidDeclaration> executables;

  // Parse cross-compile target (must match what cmd_build used).
  TargetSpec target;
  if (auto t = args.get_option("target"))
    target = parse_target(*t);
  else if (auto t = args.get_option("t"))
    target = parse_target(*t);
  selection.target_triple = selection_triple(target);
  std::string triple_dir =
      target.is_native() ? detect_host_target().triple_ : target.triple_;
  const Path out_dir = *root / "out" / triple_dir;

  for (const auto &id : outer_graph->roots) {
    auto declaration =
        read_moid_declaration(moid_declaration_path(out_dir, id.value));
    if (!declaration) {
      std::println(std::cerr, "bake: {}", declaration.error());
      return 1;
    }
    if (declaration->id != id.value) {
      std::println(std::cerr,
                   "bake: persisted declaration identity does not match "
                   "selected moid '{}'",
                   id.value);
      return 1;
    }
    if (declaration->type == MoidType::Executable)
      executables.push_back(std::move(*declaration));
  }

  if (executables.empty()) {
    std::println(std::cerr, "bake: cannot run non-executable package");
    return 1;
  }
  std::sort(
      executables.begin(), executables.end(),
      [](const auto &left, const auto &right) { return left.id < right.id; });
  if (executables.size() > 1) {
    std::string candidates;
    for (const auto &declaration : executables) {
      if (!candidates.empty())
        candidates += ", ";
      candidates += declaration.id + " (" + declaration.name + ")";
    }
    std::println(std::cerr,
                 "bake: multiple executable packages selected; use -p <member> "
                 "to choose one: {}",
                 candidates);
    return 1;
  }

  const auto &executable = executables.front();

  exe_path =
      out_dir / "bin" / library_name(executable.name, executable.type, target);

  if (!exe_path.exists()) {
    std::println(std::cerr, "bake: built executable not found at {}",
                 exe_path.string());
    return 1;
  }

  // Build command line: executable + remaining args
  std::vector<std::string> run_args;
  for (auto &p : args.positional) {
    run_args.push_back(p);
  }

  auto result = run_process(exe_path.string(), run_args, *root);
  return result.exit_code;
}

} // namespace bake::cli
