// bake.cli.compdb — the `compdb` command.

export module bake.cli.compdb;

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
import bake.cli;

namespace bake::cli {

// ===== compdb command =====

// Print/refresh compile_commands.json from the resolved build graph —
// the same view the LSP serves queries from. No build is executed.
export int cmd_compdb(const ParsedArgs &args) {
  auto root = find_project_root();
  if (!root) {
    std::println(std::cerr, "bake: no bake.toml found");
    return 1;
  }

  TargetSpec target = detect_host_target();
  if (auto t = args.get_option("target"))
    target = parse_target(*t);
  else if (auto t = args.get_option("t"))
    target = parse_target(*t);

  std::string profile = "dev";
  if (args.has_option("release"))
    profile = "release";
  if (auto p = args.get_option("profile"))
    profile = *p;

  const bool offline =
      args.has_option("offline") || args.has_option("frozen");

  auto db = build_compile_db(*root, target, profile, offline);
  if (!db) {
    std::println(std::cerr, "bake: {}", db.error());
    return 1;
  }

  Path output = *root / "compile_commands.json";
  if (!args.positional.empty())
    output = Path(args.positional[0]).absolute();

  write_compdb_json(*db, output);
  std::println("Wrote {} ({} entries)", output.string(),
               db->entries.size());
  return 0;
}

} // namespace bake::cli
