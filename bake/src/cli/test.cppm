// bake.cli.test — the `test` command.

export module bake.cli.test;

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

// ===== test command =====
//
// Orchestration layer: builds the project, then runs the test binaries that
// build.cpp registered via b.add_test(). The tests' "testness" lives inside
// the binaries; bake only discovers, builds, runs, and reports.

export int cmd_test(const ParsedArgs &args) {
  // Build first with dev profile.
  int build_result = cmd_build(args);
  if (build_result != 0)
    return build_result;

  auto root = find_project_root();
  if (!root)
    return 1;

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

  // Must match what cmd_build used.
  TargetSpec target;
  if (auto t = args.get_option("target"))
    target = parse_target(*t);
  else if (auto t = args.get_option("t"))
    target = parse_target(*t);
  selection.target_triple = selection_triple(target);
  std::string triple_dir =
      target.is_native() ? detect_host_target().triple_ : target.triple_;
  const Path out_dir = *root / "out" / triple_dir;

  // Collect registrations from the selected roots, in declaration order.
  struct Registration {
    std::string name;
    std::string binary;
    bool is_default = false;
  };
  std::vector<Registration> registrations;
  for (const auto &id : outer_graph->roots) {
    auto declaration =
        read_moid_declaration(moid_declaration_path(out_dir, id.value));
    if (!declaration) {
      std::println(std::cerr, "bake: {}", declaration.error());
      return 1;
    }
    for (const auto &test : declaration->tests)
      registrations.push_back({test.name, test.binary, test.is_default});
  }

  if (registrations.empty()) {
    std::println("bake: no tests registered");
    return 0;
  }

  // --all runs everything; a positional names one test; otherwise the
  // registration marked default runs (the last marker wins).
  std::vector<Registration> selected;
  if (args.has_option("all")) {
    selected = registrations;
  } else if (!args.positional.empty()) {
    for (const auto &registration : registrations)
      if (registration.name == args.positional[0])
        selected.push_back(registration);
    if (selected.empty()) {
      std::println(std::cerr, "bake: no test named '{}'", args.positional[0]);
      return 1;
    }
  } else {
    const Registration *default_test = nullptr;
    for (const auto &registration : registrations)
      if (registration.is_default)
        default_test = &registration;
    if (!default_test) {
      std::println(std::cerr,
                   "bake: no default test registered; use 'bake test <name>' "
                   "or --all");
      return 1;
    }
    selected.push_back(*default_test);
  }

  int failures = 0;
  for (const auto &registration : selected) {
    Path binary_path = out_dir / "bin" /
        library_name(registration.binary, MoidType::Executable, target);
    if (!binary_path.exists()) {
      std::println(std::cerr, "bake: test binary '{}' not found at {}",
                   registration.binary, binary_path.string());
      ++failures;
      continue;
    }
    std::println("   Running {}", registration.name);
    auto result = run_process(binary_path.string(), {}, *root);
    if (result.success()) {
      std::println("   Passed {}", registration.name);
    } else {
      std::println(std::cerr, "   Failed {} (exit {})", registration.name,
                   result.exit_code);
      ++failures;
    }
  }

  if (failures > 0)
    std::println(std::cerr, "bake: {} of {} selected test(s) failed", failures,
                 selected.size());
  return failures > 0 ? 1 : 0;
}

} // namespace bake::cli
