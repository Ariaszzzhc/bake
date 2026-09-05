// bake.cli.add — the `add` command.

export module bake.cli.add;

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
import bake.cli.dep_edit;
import bake.cli;

namespace bake::cli {

export int cmd_add(const ParsedArgs &args) {
  auto root = find_project_root();
  if (!root) {
    std::println(std::cerr, "bake: no bake.toml found");
    return 1;
  }

  if (args.positional.empty()) {
    std::println(std::cerr,
                 "bake: add requires a URL\n"
                 "Usage: bake add <url> [--tag <t> | --branch <b> | --rev <r>] "
                 "[name] [--target <glob>]");
    return 1;
  }

  std::string url = args.positional[0];
  // A local path is normalized to a file:// URL so the manifest always
  // holds one dependency form.
  if (!url.contains("://")) {
    if (url.starts_with('/'))
      url = "file://" + url;
    else if (url.starts_with("./") || url.starts_with("../")) {
      std::error_code error;
      auto absolute = std::filesystem::absolute(url, error);
      if (!error) url = "file://" + absolute.generic_string();
    }
  }
  std::string tag = args.get_option("tag").value_or("");
  std::string branch = args.get_option("branch").value_or("");
  std::string rev = args.get_option("rev").value_or("");
  std::string target_glob = args.get_option("target").value_or("");
  std::string name;

  const bool archive = is_archive_url(url);
  const int ref_count =
      !tag.empty() + !branch.empty() + !rev.empty();

  if (ref_count > 1) {
    std::println(std::cerr,
                 "bake: --tag, --branch and --rev are mutually exclusive");
    return 1;
  }
  if (archive && ref_count > 0) {
    std::println(std::cerr,
                 "bake: archive dependencies take no ref (the URL pins the "
                 "content)");
    return 1;
  }

  if (!target_glob.empty() && !valid_target_pattern(target_glob)) {
    std::println(std::cerr,
                 "bake: invalid target pattern '{}': '*' must match a whole "
                 "segment",
                 target_glob);
    return 1;
  }

  // Derive name from URL or use second positional
  if (args.positional.size() >= 2) {
    name = args.positional[1];
  } else if (archive) {
    std::string file = url.substr(url.find_last_of('/') + 1);
    std::string ext = archive_extension(file);
    name = file.substr(0, file.size() - ext.size());
  } else {
    std::size_t last_slash = url.find_last_of('/');
    name = (last_slash != std::string::npos) ? url.substr(last_slash + 1)
                                             : "dep";
    if (ends_with(name, ".git")) name = name.substr(0, name.size() - 4);
  }

  auto manifest = Manifest::load(*root);
  if (!manifest) {
    std::println(std::cerr, "bake: failed to load bake.toml");
    return 1;
  }

  const std::string section = target_glob.empty()
                                  ? "[dependencies]"
                                  : "[target.\"" + target_glob + "\".dependencies]";

  // OverwriteIfExplicit: an identical existing entry is skipped with a hint;
  // a different definition under the same alias is replaced.
  for (auto &[label, scope] : dependency_scopes(*manifest)) {
    auto existing = scope->find(name);
    if (existing == scope->end()) continue;
    const Dependency &dep = existing->second;
    if (dep.url == url && dep.tag == tag && dep.branch == branch &&
        dep.rev == rev) {
      std::println("bake: '{}' is already a dependency (in {}); run 'bake "
                   "update' to re-resolve",
                   name, label);
      return 0;
    }
    if (!remove_dependency_line(*root / "bake.toml", label, name)) {
      std::println(std::cerr, "bake: failed to replace '{}' in {}", name,
                   label);
      return 1;
    }
    std::println("bake: replaced dependency '{}' in {}", name, label);
    break;
  }

  std::string dep_line = name + " = { url = \"" + url + "\"";
  if (!tag.empty()) dep_line += ", tag = \"" + tag + "\"";
  if (!branch.empty()) dep_line += ", branch = \"" + branch + "\"";
  if (!rev.empty()) dep_line += ", rev = \"" + rev + "\"";
  dep_line += " }\n";

  Path toml_path = *root / "bake.toml";
  if (!insert_dependency_line(toml_path, section, dep_line)) {
    std::println(std::cerr, "bake: failed to write bake.toml");
    return 1;
  }

  if (!target_glob.empty())
    std::println("Added dependency '{}' to {} ({} ref)", name, section,
                 archive ? "archive" : ref_count == 0 ? "default-branch"
                                                      : "pinned");
  else
    std::println("Added dependency '{}' to [dependencies] ({} ref)", name,
                 archive ? "archive" : ref_count == 0 ? "default-branch"
                                                      : "pinned");
  std::println("Run 'bake build' to resolve and download.");
  return 0;
}

} // namespace bake::cli
