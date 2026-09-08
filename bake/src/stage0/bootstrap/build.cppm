// bake.stage0.build — the `build` command.

export module bake.stage0.build;

import std;
import bake.util;
import bake.toolchain.target;
import bake.stage0.project;
import bake.stage0.moid;
import bake.stage0.graph;
import bake.stage0.cmdgen;
import bake.stage0.engine;
import bake.stage0.package;
import bake.json;
import bake.stage0.cli;

namespace bake::cli {

namespace {
SourceIndex source_index_from_lock(const Path& root) {
  SourceIndex index;
  auto lockfile = Lockfile::load(root / "bake.lock");
  if (!lockfile) return index;
  const Path cache_dir = get_cache_dir();
  for (const auto& [_, dep] : lockfile->deps) {
    if (!dep.is_remote()) continue;
    LockedSource source;
    source.url = normalize_dependency_url(dep.url);
    source.ref = dep.is_archive() ? "" : dep.ref;
    source.ref_type = dep.is_archive() ? "archive" : dep.ref_type;
    source.commit = dep.commit;
    source.integrity = dep.integrity;
    source.cache_hash = dep.cache_hash();
    source.cache_dir = cache_dir / source.cache_hash;
    index.entries.push_back(std::move(source));
  }
  return index;
}
} // namespace

// ===== build command =====

export int cmd_build(const ParsedArgs &args) {
  auto root = find_project_root();
  if (!root) {
    std::println(std::cerr, "bake: no bake.toml found");
    return 1;
  }

  auto manifest = Manifest::load(*root);
  if (!manifest) {
    std::println(std::cerr, "bake: failed to load bake.toml");
    return 1;
  }

  // Lockfile enforcement: resolve deps, verify cache, enforce
  // --locked/--offline.
  if (enforce_lock(*root, *manifest, args) != 0)
    return 1;

  auto selected_member =
      resolve_workspace_member_selection(*manifest, args.get_option("p"));
  if (!selected_member) {
    std::println(std::cerr, "bake: {}", selected_member.error());
    return 1;
  }

  auto root_features = parse_root_features(*manifest, args, *selected_member);
  if (!root_features)
    return 1;

  BuildSelection selection;
  selection.workspace_member = std::move(*selected_member);
  selection.root_features = std::move(*root_features);
  TargetSpec selection_target;
  if (auto t = args.get_option("target"))
    selection_target = parse_target(*t);
  else if (auto t = args.get_option("t"))
    selection_target = parse_target(*t);
  selection.target_triple = selection_triple(selection_target);

  ResolvePolicy policy;
  policy.offline = args.has_option("offline") || args.has_option("frozen");

  auto outer_graph = resolve_moid_graph(*manifest, selection, policy,
                          source_index_from_lock(*root));
  if (!outer_graph) {
    std::println(std::cerr, "bake: {}", outer_graph.error());
    return 1;
  }

  // Profile selection.
  std::string profile = "dev";
  if (args.has_option("release"))
    profile = "release";
  if (auto p = args.get_option("profile"))
    profile = *p;

  // Validate profile name (built-in or user-declared)
  if (profile != "dev" && profile != "release") {
    bool found = false;
    if (manifest) {
      for (const auto& [name, _] : manifest->profiles)
        if (name == profile) { found = true; break; }
    }
    if (!found) {
      std::println(std::cerr,
          "bake: unknown profile '{}' (declare it in bake.toml or use "
          "'dev'/'release')", profile);
      return 1;
    }
  }

  TargetSpec target = detect_host_target();

  // Cross-compile target from --target=<triple> or -t <triple>.
  {
    auto target_spec = args.get_option("target");
    if (!target_spec)
      target_spec = args.get_option("t");
    if (target_spec) {
      // Validate raw input has exactly 3 segments before parse_target
      // normalizes away vendor segments like "unknown".
      int dash_count = static_cast<int>(
          std::count(target_spec->begin(), target_spec->end(), '-'));
      if (dash_count != 2) {
        std::println(std::cerr,
                     "bake: invalid target triple '{}': expected exactly 3 "
                     "segments (arch-os-libc), got {}",
                     *target_spec, dash_count + 1);
        return 1;
      }
      target = parse_target(*target_spec);
    }
  }

  // Output directory with target isolation.
  std::string triple_dir =
      target.is_native() ? detect_host_target().triple_ : target.triple_;
  Path out_dir = *root / "out" / triple_dir / profile;
  Path bake_dir = out_dir / ".bake";
  bake_dir.mkdir_recursive();

  const std::string label =
      manifest->is_workspace()
          ? root->filename_string().empty() ? root->string()
                                            : root->filename_string()
          : (manifest->has_moid()
                 ? manifest->moid->name + " v" + manifest->moid->version
                 : "project");

  std::println("   Building {}", label);

  // The std module is a compiler concern — the bake c++ shim provisions
  // and injects it. The build system deliberately knows nothing about it.


  auto configured =
      configure_moid_graph(*outer_graph, target, out_dir, *root, profile);
  if (!configured) {
    std::println(std::cerr, "bake: {}", configured.error());
    return 1;
  }

  auto graph = build_graph(*outer_graph, target, out_dir, *root);
  if (!graph) {
    std::println(std::cerr, "bake: {}", graph.error());
    return 1;
  }

  // Persist the bootstrap build graph for incremental execution.
  write_graph_json(*graph, bake_dir / "graph.json");

  // Execute.
  int jobs = 0;
  if (auto j = args.get_option("j"))
    jobs = std::atoi(j->c_str());
  bool verbose = args.has_option("v") || args.has_option("verbose");

  int result = execute_graph(*graph, jobs, verbose);
  return result;
}

} // namespace bake::cli
