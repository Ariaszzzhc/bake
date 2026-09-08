// bake.stage0.update — the `update` command.

export module bake.stage0.update;

import std;
import bake.util;
import bake.stage0.project;
import bake.stage0.package;
import bake.stage0.cli;

namespace bake::cli {

// ===== update command =====

export int cmd_update(const ParsedArgs &args) {
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

  // Root + workspace members all carry dependency scopes.
  std::vector<Manifest> manifests;
  manifests.push_back(*manifest);
  if (manifest->is_workspace()) {
    for (const auto &member : manifest->workspace->members) {
      Path member_dir = (*root / member.c_str()).lexically_normal();
      auto sub = Manifest::load(member_dir);
      if (sub) {
        sub->project_dir = member_dir;
        manifests.push_back(std::move(*sub));
      }
    }
  }

  bool has_any_dep = false;
  for (const auto &m : manifests) {
    has_any_dep = has_any_dep || !m.dependencies.empty();
    for (const auto &condition : m.targets)
      has_any_dep = has_any_dep || !condition.dependencies.empty();
  }
  if (!has_any_dep) {
    std::println("bake: no dependencies to update");
    return 0;
  }

  // Determine which dep to update (empty = all), searching every scope.
  std::string filter_dep;
  const Dependency *filter = nullptr;
  if (!args.positional.empty()) {
    filter_dep = args.positional[0];
    auto find_scope =
        [&](const std::map<std::string, Dependency> &deps)
            -> const Dependency * {
      auto it = deps.find(filter_dep);
      return it == deps.end() ? nullptr : &it->second;
    };
    for (const auto &m : manifests) {
      if (filter) break;
      filter = find_scope(m.dependencies);
      for (const auto &condition : m.targets) {
        if (filter) break;
        filter = find_scope(condition.dependencies);
      }
    }
    if (!filter) {
      std::println(std::cerr, "bake: dependency '{}' not found in bake.toml",
                   filter_dep);
      return 1;
    }
    if (filter->is_path_dep) {
      std::println(std::cerr,
                   "bake: '{}' is a path dependency (no resolution needed)",
                   filter_dep);
      return 1;
    }
  }

  Path lock_path = *root / "bake.lock";

  // Load existing lock for comparison
  auto old_lock = Lockfile::load(lock_path);

  // Hints: with a filter, carry every locked entry except the filtered
  // dependency's URL (forced re-resolution); without a filter, re-resolve
  // the full closure — the only path that moves locked refs.
  std::optional<Lockfile> hints;
  if (filter && old_lock) {
    hints = *old_lock;
    const std::string forced_url = normalize_dependency_url(filter->url);
    std::erase_if(hints->deps, [&](const auto &pair) {
      return normalize_dependency_url(pair.second.url) == forced_url;
    });
  }

  std::set<std::string> cli_features;
  {
    auto names = parse_root_features(*manifest, args, std::nullopt);
    if (!names)
      return 1;
    cli_features.insert(names->begin(), names->end());
  }
  Resolver resolver;
  auto new_lock = resolver.resolve(*manifest, ResolverConfig{},
                                   hints ? &*hints : nullptr, cli_features);
  if (!new_lock) {
    std::println(std::cerr, "bake: failed to resolve dependencies");
    return 1;
  }

  // Report changes for the filtered dep (or all if no filter)
  auto find_entry = [&](const Lockfile *lock,
                        const Dependency &dep) -> const LockDep * {
    if (!lock) return nullptr;
    std::string ref_type = dep.is_archive() ? "archive" : "";
    std::string ref;
    if (ref_type.empty()) {
      auto pair = dep.git_ref();
      ref_type = pair.first;
      ref = pair.second;
    }
    if (!dep.version.empty()) return lock->find_tag_entry(dep.url);
    return lock->find_remote(dep.url, ref_type, ref);
  };
  auto report_change = [&](const std::string &name,
                           const Dependency &dep) {
    const LockDep *old_entry = find_entry(old_lock ? &*old_lock : nullptr, dep);
    const LockDep *new_entry = find_entry(&*new_lock, dep);

    if (!old_entry && new_entry) {
      std::println("bake: {} added", name);
    } else if (old_entry && new_entry) {
      const std::string &old_id = old_entry->commit.empty()
                                      ? old_entry->integrity
                                      : old_entry->commit;
      const std::string &new_id = new_entry->commit.empty()
                                      ? new_entry->integrity
                                      : new_entry->commit;
      if (old_id != new_id) {
        std::println("bake: {} updated: {} → {}", name,
                     old_id.substr(0, 12), new_id.substr(0, 12));
      } else {
        std::println("bake: {} unchanged ({})", name, new_id.substr(0, 12));
      }
    }
  };

  if (filter_dep.empty()) {
    for (const auto &m : manifests) {
      for (auto &[name, dep] : m.dependencies) {
        if (dep.is_path_dep) continue;
        report_change(name, dep);
      }
      for (const auto &condition : m.targets)
        for (auto &[name, dep] : condition.dependencies)
          if (!dep.is_path_dep) report_change(name, dep);
    }
  } else {
    report_change(filter_dep, *filter);
  }

  if (!new_lock->save(lock_path)) {
    std::println(std::cerr, "bake: failed to write bake.lock");
    return 1;
  }

  std::println("bake: lock file updated");
  return 0;
}

} // namespace bake::cli
