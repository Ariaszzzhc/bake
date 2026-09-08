// bake.cli.remove — the `remove` command.

export module bake.cli.remove;

import std;
import bake.util;
import bake.buildsystem.project;
import bake.buildsystem.package;
import bake.cli.dep_edit;
import bake.cli;

namespace bake::cli {

export int cmd_remove(const ParsedArgs &args) {
  auto root = find_project_root();
  if (!root) {
    std::println(std::cerr, "bake: no bake.toml found");
    return 1;
  }

  if (args.positional.empty()) {
    std::println(std::cerr,
                 "bake: remove requires a dependency name\n"
                 "Usage: bake remove <name> [--target <glob>]");
    return 1;
  }

  std::string alias = args.positional[0];
  std::string target_glob = args.get_option("target").value_or("");
  if (!target_glob.empty() && !valid_target_pattern(target_glob)) {
    std::println(std::cerr,
                 "bake: invalid target pattern '{}': '*' must match a whole "
                 "segment",
                 target_glob);
    return 1;
  }

  auto manifest = Manifest::load(*root);
  if (!manifest) {
    std::println(std::cerr, "bake: failed to load bake.toml");
    return 1;
  }

  // Resolve the scope to remove from.
  auto scopes = dependency_scopes(*manifest);
  std::string section;
  if (!target_glob.empty()) {
    section = "[target.\"" + target_glob + "\".dependencies]";
    bool found = false;
    for (auto &[label, scope] : scopes)
      if (label == section && scope->contains(alias)) found = true;
    if (!found) {
      std::string where;
      for (auto &[label, scope] : scopes)
        if (scope->contains(alias)) where += "\n  " + label;
      if (where.empty())
        std::println(std::cerr, "bake: no dependency '{}' in {}", alias,
                     section);
      else
        std::println(std::cerr,
                     "bake: '{}' is not declared in {} — it exists in:{}",
                     alias, section, where);
      return 1;
    }
  } else {
    std::vector<std::string> hits;
    for (auto &[label, scope] : scopes)
      if (scope->contains(alias)) hits.push_back(label);
    if (hits.empty()) {
      std::println(std::cerr, "bake: no dependency '{}' in bake.toml", alias);
      return 1;
    }
    if (hits.size() > 1) {
      std::string where;
      for (auto &label : hits) where += "\n  " + label;
      std::println(std::cerr,
                   "bake: '{}' is declared in multiple scopes — pick one "
                   "with --target:{}",
                   alias, where);
      return 1;
    }
    section = hits.front();
  }

  Path toml_path = *root / "bake.toml";
  if (!remove_dependency_line(toml_path, section, alias)) {
    std::println(std::cerr, "bake: failed to remove '{}' from {}", alias,
                 section);
    return 1;
  }
  std::println("Removed dependency '{}' from {}", alias, section);

  // Prune lock entries no longer referenced by any scope. Zero network;
  // the content cache is never touched.
  Path lock_path = *root / "bake.lock";
  if (auto lock = Lockfile::load(lock_path)) {
    auto updated = Manifest::load(*root);
    if (updated) {
      Lockfile pruned = prune_lock(*updated, *lock);
      if (pruned.deps.size() != lock->deps.size()) {
        if (pruned.save(lock_path)) {
          std::println("Pruned {} lock {}",
                       lock->deps.size() - pruned.deps.size(),
                       lock->deps.size() - pruned.deps.size() == 1
                           ? "entry"
                           : "entries");
        }
      }
    }
  }

  return 0;
}

} // namespace bake::cli
