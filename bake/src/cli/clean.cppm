// bake.cli.clean — the `clean` command.

export module bake.cli.clean;

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

export int cmd_clean(const ParsedArgs &args) {
  auto root = find_project_root();
  if (!root) {
    std::println(std::cerr, "bake: no bake.toml found");
    return 1;
  }

  int removed = 0;

  // Remove unified output directory
  Path out_dir = *root / "out";
  if (out_dir.exists()) {
    out_dir.remove_all();
    std::println("Removed {}", out_dir.string());
    removed++;
  }

  if (removed == 0) {
    std::println("Nothing to clean");
  }

  return 0;
}

} // namespace bake::cli
