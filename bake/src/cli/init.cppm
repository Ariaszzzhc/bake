// bake.cli.init — the `init` command.

export module bake.cli.init;

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

// ===== init command =====

export int cmd_init(const ParsedArgs &args) {
  // Determine project name. A positional argument may be a path:
  // "bake init some/dir/hello" scaffolds at that path and names the project
  // after the last component.
  std::string given;
  if (!args.positional.empty()) given = args.positional[0];

  std::string project_name;
  if (!given.empty()) {
    std::filesystem::path given_path(given);
    if (given_path.filename().empty())  // trailing separator
      given_path = given_path.parent_path();
    project_name = given_path.filename().string();
  } else {
    project_name = Path::current().filename_string();
  }
  if (project_name == "." || project_name == "..")
    project_name.clear();
  if (project_name.empty()) {
    if (given.empty()) {
      std::println(std::cerr, "bake: could not determine project name. "
                              "Please specify: bake init <name>");
    } else {
      std::println(std::cerr, "bake: invalid project name '{}'", given);
    }
    return 1;
  }
  if (!is_portable_name(project_name)) {
    std::println(std::cerr,
                 "bake: invalid project name '{}': expected a portable ASCII "
                 "name matching [A-Za-z0-9][A-Za-z0-9._+@-]*",
                 project_name);
    return 1;
  }

  // Determine Moid type
  MoidType moid_type = MoidType::Executable;
  if (auto type_str = args.get_option("type")) {
    auto parsed_type = parse_moid_type(*type_str);
    if (!parsed_type) {
      std::println(std::cerr, "bake: {}", parsed_type.error());
      return 1;
    }
    moid_type = *parsed_type;
  }

  // Determine std version
  std::string std_ver = "c++20";
  if (auto std_opt = args.get_option("std")) {
    std_ver = *std_opt;
  }

  // Determine target directory
  Path target_dir;
  bool create_subdir = false;
  if (!given.empty()) {
    target_dir = Path::current() / given;
    create_subdir = true;
    if (target_dir.exists()) {
      std::println(std::cerr, "bake: directory '{}' already exists", given);
      return 1;
    }
  } else {
    target_dir = Path::current();
  }

  // Create directories
  (target_dir / "src").mkdir_recursive();
  (target_dir / "public").mkdir_recursive();
  (target_dir / "tests").mkdir_recursive();

  // Write bake.toml. The language standard lives in [language]; only that
  // table feeds the manifest parser.
  std::string toml_content = std::string("[package]\n") + "name = \"" +
                             project_name + "\"\n" + "version = \"0.1.0\"\n" +
                             "type = \"" +
                             std::string(moid_type_str(moid_type)) +
                             "\"\n\n";
  if (is_c_standard(std_ver)) {
    toml_content += "[language]\nc = \"" + std_ver + "\"\n";
  } else {
    toml_content += "[language]\ncxx = \"" + std_ver + "\"\n";
  }

  write_file(target_dir / "bake.toml", toml_content);

  // Write source file. A C standard requests a genuine C scaffold.
  const bool use_c = is_c_standard(std_ver);
  if (moid_type == MoidType::Executable) {
    if (use_c) {
      std::string main_c = std::string("#include <stdio.h>\n\n"
                                       "int main(void) {\n"
                                       "    puts(\"Hello from " +
                                       project_name +
                                       "!\");\n"
                                       "    return 0;\n"
                                       "}\n");
      write_file(target_dir / "src" / "main.c", main_c);
    } else {
      std::string main_cpp = std::string("#include <iostream>\n\n"
                                         "int main() {\n"
                                         "    std::cout << \"Hello from " +
                                         project_name +
                                         "!\\n\";\n"
                                         "    return 0;\n"
                                         "}\n");
      write_file(target_dir / "src" / "main.cpp", main_cpp);
    }
  } else {
    std::string lib_source =
        std::string("// " + project_name +
                    " library\n\n"
                    "// Define your library functions here\n");
    write_file(target_dir / "src" / (use_c ? "lib.c" : "lib.cpp"), lib_source);

    // Public header
    std::string public_dir = "public/" + project_name;
    (target_dir / public_dir).mkdir_recursive();
    std::string header = std::string("#pragma once\n\n"
                                     "// " +
                                     project_name + " public API\n");
    write_file(target_dir / public_dir /
                   (project_name + (use_c ? ".h" : ".hpp")),
               header);
  }

  // Write .gitignore
  write_file(target_dir / ".gitignore", "/out/\n"
                                        "compile_commands.json\n");

  // Print success
  if (create_subdir) {
    std::println("Created project '{}' in ./{}", project_name, given);
    std::println("  Next: cd {} && bake build", given);
  } else {
    std::println("Initialized project '{}' in current directory", project_name);
    std::println("  Next: bake build");
  }

  return 0;
}

// (source_index_from_lock lives in bake.buildsystem.query — the
// lockfile→SourceIndex bridge shared by build, run, test, compdb, lsp.)


} // namespace bake::cli
