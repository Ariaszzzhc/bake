// bake — multicall binary: toolchain driver (cc/c++/ar/-cc1) + build system
// commands. Composition root; there is no separate CLI module.

import std;
import bake.util;
import bake.toolchain.target;
import bake.toolchain.llvm;
import bake.toolchain.runtime;
import bake.buildsystem.project;
import bake.buildsystem.moid;
import bake.buildsystem.graph;
import bake.buildsystem.cmdgen;
import bake.buildsystem.engine;
import bake.buildsystem.package;
import tomlplusplus;

#ifndef BAKE_VERSION
#define BAKE_VERSION "0.1.0"
#endif

namespace bake::cli {

// ===== Argument parsing =====

struct ParsedArgs {
  std::string command;                 // subcommand (empty if none)
  std::vector<std::string> positional; // positional arguments
  std::vector<std::string> options;    // --flag or --key=value
  bool help = false;
  bool version = false;

  // Get an option value (--key=value, --key value, -k value, or -k=value)
  std::optional<std::string> get_option(std::string_view name) const {
    std::string long_prefix = std::string("--") + std::string(name) + "=";
    std::string short_prefix = std::string("-") + std::string(name) + "=";
    for (auto &opt : options) {
      if (opt == long_prefix.substr(0, long_prefix.size() - 1) ||
          opt == short_prefix.substr(0, short_prefix.size() - 1)) {
        return std::string("true"); // flag without value
      }
      if (starts_with(opt, long_prefix)) {
        return opt.substr(long_prefix.size());
      }
      if (starts_with(opt, short_prefix)) {
        return opt.substr(short_prefix.size());
      }
    }
    return std::nullopt;
  }

  bool has_option(std::string_view name) const {
    std::string long_flag = std::string("--") + std::string(name);
    std::string short_flag = std::string("-") + std::string(name);
    std::string long_prefix = long_flag + "=";
    std::string short_prefix = short_flag + "=";
    for (auto &opt : options) {
      if (opt == long_flag || opt == short_flag)
        return true;
      if (starts_with(opt, long_prefix) || starts_with(opt, short_prefix))
        return true;
    }
    return false;
  }
};

ParsedArgs parse_args(int argc, char *argv[]) {
  ParsedArgs args;

  // Options that consume the next argument as a value
  auto takes_value = [](std::string_view opt) {
    return opt == "--tag" || opt == "--type" || opt == "--std" ||
           opt == "--target" || opt == "--glibc-version" || opt == "--option" ||
           opt == "--profile" || opt == "-j" || opt == "-p" || opt == "-t";
  };

  int i = 1;
  while (i < argc) {
    std::string arg = argv[i];

    if (arg == "--help" || arg == "-h") {
      args.help = true;
    } else if (arg == "--version" || arg == "-V") {
      args.version = true;
    } else if (starts_with(arg, "--") ||
               (arg.size() > 1 && arg[0] == '-' && arg[1] != '\0')) {
      // It's an option
      if (contains(arg, "=")) {
        args.options.push_back(arg);
      } else if (takes_value(arg)) {
        if (i + 1 < argc) {
          args.options.push_back(arg + "=" + argv[i + 1]);
          ++i;
        } else {
          // Missing value — push as empty to trigger validation errors
          args.options.push_back(std::string(arg) + "=");
        }
      } else {
        args.options.push_back(arg);
      }
    } else if (args.command.empty()) {
      args.command = arg;
    } else {
      args.positional.push_back(arg);
    }
    ++i;
  }

  return args;
}

namespace {

struct OptionOverride {
  std::string name;
  std::optional<std::string> value;
};

std::optional<std::vector<OptionOverride>>
parse_option_overrides(const ParsedArgs &args) {
  std::vector<OptionOverride> overrides;
  for (const auto &option : args.options) {
    if (!starts_with(option, "--option="))
      continue;

    const std::string declaration = option.substr(9);
    if (declaration.empty()) {
      std::println(std::cerr,
                   "bake: --option requires <name> or <name>=<value>");
      return std::nullopt;
    }

    const std::size_t equals = declaration.find('=');
    OptionOverride override;
    override.name = declaration.substr(0, equals);
    if (override.name.empty()) {
      std::println(std::cerr, "bake: build option name cannot be empty");
      return std::nullopt;
    }
    if (equals != std::string::npos) {
      override.value = declaration.substr(equals + 1);
    }
    overrides.push_back(std::move(override));
  }
  return overrides;
}

std::optional<bool> parse_bool_option(std::string_view value) {
  if (value == "true" || value == "1" || value == "on" || value == "yes")
    return true;
  if (value == "false" || value == "0" || value == "off" || value == "no")
    return false;
  return std::nullopt;
}

std::optional<BuildOption>
parse_root_option_value(const OptionOverride &override,
                        const BuildOption &declared) {
  if (!override.value)
    return BuildOption{true};
  auto value = parse_bool_option(*override.value);
  if (!value) {
    std::println(std::cerr, "bake: option '{}' expects a boolean, got '{}'",
                 override.name, *override.value);
    return std::nullopt;
  }
  return BuildOption{*value};
}

std::optional<std::map<std::string, BuildOption>>
parse_root_options(const Manifest &root, const ParsedArgs &args,
                   const std::optional<std::string> &selected_member) {
  auto overrides = parse_option_overrides(args);
  if (!overrides)
    return std::nullopt;
  if (overrides->empty())
    return std::map<std::string, BuildOption>{};

  std::vector<Manifest> selected_roots;
  if (root.is_workspace()) {
    if (selected_member) {
      auto manifest =
          Manifest::load(root.project_dir / selected_member->c_str());
      if (manifest && manifest->has_moid()) {
        selected_roots.push_back(std::move(*manifest));
      }
    } else {
      for (const auto &member : root.workspace->members) {
        auto manifest = Manifest::load(root.project_dir / member.c_str());
        if (!manifest || !manifest->has_moid())
          continue;
        selected_roots.push_back(std::move(*manifest));
      }
    }
  } else if (root.has_moid()) {
    selected_roots.push_back(root);
  }

  if (selected_roots.empty()) {
    std::println(
        std::cerr,
        "bake: cannot apply build options without a selected root moid");
    return std::nullopt;
  }

  std::map<std::string, BuildOption> result;
  for (const auto &override : *overrides) {
    auto declared = selected_roots.front().options.find(override.name);
    if (declared == selected_roots.front().options.end()) {
      std::println(std::cerr,
                   "bake: unknown build option '{}' (declare it in [options])",
                   override.name);
      return std::nullopt;
    }
    auto value = parse_root_option_value(override, declared->second);
    if (!value)
      return std::nullopt;
    result[override.name] = std::move(*value);
  }
  return result;
}

} // namespace

// ===== Help text =====

void print_version() { std::println("bake {}", BAKE_VERSION); }

void print_help() {
  std::println(
      "bake {} — All-in-One C/C++ toolchain\n"
      "\n"
      "USAGE:\n"
      "    bake <subcommand> [options]\n"
      "    bake <subcommand> --help\n"
      "\n"
      "SUBCOMMANDS:\n"
      "    init [name]     Create a new project scaffold\n"
      "    build           Build the project\n"
      "    add <url>       Add a dependency to bake.toml\n"
      "    update [dep]    Re-resolve dependencies and update bake.lock\n"
      "    run             Build and run the executable\n"
      "    test            Build and run tests\n"
      "    clean           Remove build artifacts\n"
      "    cc              Invoke embedded Clang C driver\n"
      "    c++             Invoke embedded Clang C++ driver\n"
      "    ar              Create static archive\n"
      "    audit           Verify toolchain self-containment\n"
      "\n"
      "GLOBAL OPTIONS:\n"
      "    -V, --version   Print version and exit\n"
      "    -h, --help      Print this help and exit\n"
      "\n"
      "BUILD OPTIONS:\n"
      "    --option <name>[=value]  Override a [options] value from bake.toml\n"
      "    --target=<triple>       Cross-compile target\n"
      "    --release               Build with release profile\n"
      "    --profile <name>        Use a specific build profile\n"
      "    --locked                Fail if lock is missing or stale\n"
      "    --offline               Never connect to the network\n"
      "    --frozen                Equivalent to --locked --offline\n"
      "    -v, --verbose           Show per-file compile progress\n"
      "    -p <member>             Build a specific workspace member\n"
      "    -j <n>                  Parallel job count\n"
      "\n"
      "For more information, visit: https://github.com/arias/bake",
      BAKE_VERSION);
}

void print_command_help(std::string_view cmd) {
  if (cmd == "init") {
    std::println(
        "bake init — Create a new project scaffold\n"
        "\n"
        "USAGE:\n"
        "    bake init [name] [options]\n"
        "\n"
        "OPTIONS:\n"
        "    --type <executable|lib|dylib>  Moid type (default: executable)\n"
        "    --std <c11|c17|c23|c++17|c++20|c++23>      Language standard "
        "(default: c++20)\n"
        "\n"
        "If [name] is omitted, scaffolds in the current directory.");
  } else if (cmd == "build") {
    std::println(
        "bake build — Build the project\n"
        "\n"
        "USAGE:\n"
        "    bake build [options]\n"
        "\n"
        "OPTIONS:\n"
        "    --option <name>[=value]  Override a [options] value\n"
        "    -j <n>                   Parallel job count\n"
        "    -p <member>              Build specific workspace member\n"
        "    --release                Build with release profile\n"
        "    --profile <name>         Build with specific profile\n"
        "    --locked                 Fail if lock is missing/stale\n"
        "    --offline                No network access\n"
        "    --frozen                 --locked + --offline\n"
        "    -v, --verbose            Show per-file compile progress");
  } else if (cmd == "test") {
    std::println(
        "bake test — Build and run registered tests\n"
        "\n"
        "USAGE:\n"
        "    bake test [name] [options]\n"
        "\n"
        "Runs the default test registered via build.cpp (b.add_test()), the\n"
        "named test, or every test with --all. Exits non-zero if any test\n"
        "fails.\n"
        "\n"
        "OPTIONS:\n"
        "    --all                     Run every registered test\n"
        "    -p <member>               Test specific workspace member\n"
        "    -j <n>                    Parallel job count for the build");
  } else if (cmd == "clean") {
    std::println("bake clean — Remove build artifacts\n"
                 "\n"
                 "USAGE:\n"
                 "    bake clean");
  } else if (cmd == "audit") {
    std::println(
        "bake audit — Verify toolchain self-containment\n"
        "\n"
        "USAGE:\n"
        "    bake audit\n"
        "\n"
        "Checks that bake's compiler produces binaries without relying\n"
        "on system SDK headers or libraries beyond the kernel interface.");
  } else {
    print_help();
  }
}

// ===== init command =====

int cmd_init(const ParsedArgs &args) {
  // Determine project name
  std::string project_name;
  if (!args.positional.empty()) {
    project_name = args.positional[0];
  } else {
    // Use current directory name
    auto cwd = Path::current();
    project_name = cwd.filename_string();
    if (project_name.empty() || project_name == "/") {
      std::println(std::cerr, "bake: could not determine project name. Please "
                              "specify: bake init <name>");
      return 1;
    }
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
  if (!args.positional.empty()) {
    target_dir = Path::current() / project_name;
    create_subdir = true;
    if (target_dir.exists()) {
      std::println(std::cerr, "bake: directory '{}' already exists",
                   project_name);
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
    std::println("Created project '{}' in ./{}", project_name, project_name);
    std::println("  Next: cd {} && bake build", project_name);
  } else {
    std::println("Initialized project '{}' in current directory", project_name);
    std::println("  Next: bake build");
  }

  return 0;
}

// ===== Lock enforcement =====

// Check and resolve the lockfile before building. Returns 0 if the lock is
// consistent or was successfully (re)resolved. Returns non-zero on failure.
// On success, root/bake.lock is guaranteed to exist and be consistent.
//
// --locked/--frozen: full enforcement — consistency check + cache verification.
// --offline (without --locked): skip — let resolve_moid_graph produce specific
//   ownership diagnostics from the lockfile state.
// Normal: resolve if needed, verify/redownload cache.
int enforce_lock(const Path &root, const Manifest &manifest,
                 const ParsedArgs &args) {
  if (Lockfile::has_only_path_deps(manifest))
    return 0;

  bool offline = args.has_option("offline") || args.has_option("frozen");
  bool locked = args.has_option("locked") || args.has_option("frozen");

  Path lock_path = root / "bake.lock";
  auto lockfile = Lockfile::load(lock_path);

  bool needs_resolve = !lockfile || !lockfile->is_consistent(manifest, root);

  // For --locked/--frozen: full enforcement.
  if (locked) {
    if (needs_resolve) {
      std::println(std::cerr, "bake: lock file missing or stale (--locked)");
      return 1;
    }
    if (lockfile && !verify_lock_cache(*lockfile, get_cache_dir())) {
      std::println(std::cerr, "bake: cache verification failed — cached "
                              "sources missing or corrupted");
      return 1;
    }
    return 0;
  }

  // For --offline without --locked: don't intercept. Let resolve_moid_graph
  // produce specific ownership diagnostics from the lockfile.
  if (offline)
    return 0;

  // Normal build: verify cache when lock is consistent.
  if (!needs_resolve && lockfile) {
    if (!verify_lock_cache(*lockfile, get_cache_dir())) {
      Resolver resolver;
      if (!resolver.redownload(*lockfile, ResolverConfig{}) ||
          !verify_lock_cache(*lockfile, get_cache_dir())) {
        std::println(std::cerr, "bake: re-download failed. Run 'bake update' "
                                "to re-resolve from scratch.");
        return 1;
      }
    }
    return 0;
  }

  // Normal build, lock missing/stale: resolve dependencies.
  Resolver resolver;
  auto new_lock = resolver.resolve(manifest, ResolverConfig{});
  if (!new_lock) {
    std::println(std::cerr, "bake: failed to resolve dependencies");
    return 1;
  }
  if (!new_lock->save(lock_path)) {
    std::println(std::cerr, "bake: failed to write bake.lock");
    return 1;
  }
  return 0;
}

// ===== build command =====

int cmd_build(const ParsedArgs &args) {
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

  auto root_options = parse_root_options(*manifest, args, *selected_member);
  if (!root_options)
    return 1;

  BuildSelection selection;
  selection.workspace_member = std::move(*selected_member);
  selection.root_options = std::move(*root_options);

  ResolvePolicy policy;
  policy.locked = args.has_option("locked") || args.has_option("frozen");
  policy.offline = args.has_option("offline") || args.has_option("frozen");

  auto outer_graph = resolve_moid_graph(*manifest, selection, policy);
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
  Path out_dir = *root / "out" / triple_dir;
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

  // Std PCM for user code. build.cpp handles its own native PCM internally.
  ModuleFileMap prebuilt_modules = ensure_std_modules(target, out_dir);

  auto configured =
      configure_moid_graph(*outer_graph, target, out_dir, *root, profile);
  if (!configured) {
    std::println(std::cerr, "bake: {}", configured.error());
    return 1;
  }

  auto graph = build_graph(*outer_graph, target, out_dir, *root, prebuilt_modules);
  if (!graph) {
    std::println(std::cerr, "bake: {}", graph.error());
    return 1;
  }

  // Write graph.json and compile_commands.json.
  write_graph_json(*graph, bake_dir / "graph.json");
  write_compile_commands(*graph, *root / "compile_commands.json");

  // Execute.
  int jobs = 0;
  if (auto j = args.get_option("j"))
    jobs = std::atoi(j->c_str());
  bool verbose = args.has_option("v") || args.has_option("verbose");

  int result = execute_graph(*graph, jobs, verbose);
  return result;
}

int cmd_clean(const ParsedArgs &args) {
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

// ===== audit command =====
//
// Verifies that bake's compiler produces binaries without relying on
// system SDK headers or libraries beyond the kernel interface (dyld,
// libSystem.B.dylib on macOS).

int cmd_audit(const ParsedArgs &args) {
  Path exe = get_self_exe_path();
  if (exe.string().empty())
    exe = "bake";
  Path lib_dir = find_lib_dir();
  std::string lib_dir_str = lib_dir.string();

  // Create temp source files.
  Path tmp_dir = std::filesystem::temp_directory_path() / "bake-audit";
  tmp_dir.mkdir_recursive();
  Path src_c = tmp_dir / "audit.c";
  Path src_cpp = tmp_dir / "audit.cpp";
  Path out_c = tmp_dir / "audit_c";
  Path out_cpp = tmp_dir / "audit_cpp";
  write_file(src_c, "int main(void) { return 0; }\n");
  write_file(src_cpp, "#include <iostream>\n"
                      "int main() { std::cout << \"ok\"; return 0; }\n");

  int violations = 0;

  // ── Check 1: compile C, verify it works ──
  {
    std::vector<std::string> cmd = {exe.string(), "cc", src_c.string(), "-o",
                                    out_c.string()};
    auto r = run_process(cmd, Path(), true);
    if (!r.success()) {
      std::println(std::cerr, "FAIL: bake cc failed to compile test program");
      std::print(std::cerr, "{}", r.stderr_output);
      return 1;
    }
  }

  // ── Check 2: compile C++, verify it works ──
  {
    std::vector<std::string> cmd = {exe.string(), "c++", src_cpp.string(), "-o",
                                    out_cpp.string()};
    auto r = run_process(cmd, Path(), true);
    if (!r.success()) {
      std::println(std::cerr, "FAIL: bake c++ failed to compile test program");
      std::print(std::cerr, "{}", r.stderr_output);
      return 1;
    }
  }

  // ── Check 3: inspect output binary dependencies ──
  auto host = detect_host_target();
  if (host.is_darwin() && out_c.exists()) {
    auto r = run_process({"otool", "-L", out_c.string()}, Path(), true);
    if (r.success()) {
      std::istringstream ss(r.stdout_output);
      std::string line;
      while (std::getline(ss, line)) {
        // Whitelist: the binary itself, dyld, libSystem.
        if (line.find(out_c.filename().string()) != std::string::npos)
          continue;
        if (line.find("/usr/lib/libSystem.B.dylib") != std::string::npos)
          continue;
        if (line.find("/usr/lib/dyld") != std::string::npos)
          continue;
        // Any other dynamic dependency is a violation.
        if (line.find(".dylib") != std::string::npos ||
            line.find(".so") != std::string::npos) {
          std::println(std::cerr, "  VIOLATION: unexpected dynamic dep: {}",
                       std::string_view(line).substr(0, 120));
          violations++;
        }
      }
    }
  }

  // Clean up.
  src_c.remove();
  src_cpp.remove();
  out_c.remove();
  out_cpp.remove();
  tmp_dir.remove();

  if (violations == 0) {
    std::println("PASS: toolchain is self-contained");
    return 0;
  } else {
    std::println(std::cerr, "FAIL: {} violation(s) found", violations);
    return 1;
  }
}

// ===== run command =====

int cmd_run(const ParsedArgs &args) {
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
  policy.locked = args.has_option("locked") || args.has_option("frozen");
  policy.offline = args.has_option("offline") || args.has_option("frozen");
  auto outer_graph = resolve_moid_graph(*manifest, selection, policy);
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

// ===== add command =====

int cmd_add(const ParsedArgs &args) {
  auto root = find_project_root();
  if (!root) {
    std::println(std::cerr, "bake: no bake.toml found");
    return 1;
  }

  if (args.positional.empty()) {
    std::println(std::cerr, "bake: add requires a URL");
    std::println(std::cerr, "Usage: bake add <url> --tag <tag> [name]");
    return 1;
  }

  std::string url = args.positional[0];
  std::string tag = args.get_option("tag").value_or("");
  std::string name;

  // Derive name from URL or use second positional
  if (args.positional.size() >= 2) {
    name = args.positional[1];
  } else {
    // Extract repo name from URL
    // e.g., https://github.com/fmtlib/fmt → fmt
    std::size_t last_slash = url.find_last_of('/');
    if (last_slash != std::string::npos) {
      name = url.substr(last_slash + 1);
      // Remove .git suffix
      if (ends_with(name, ".git")) {
        name = name.substr(0, name.size() - 4);
      }
    } else {
      name = "dep";
    }
  }

  if (tag.empty()) {
    std::println(std::cerr, "bake: add requires --tag <tag>");
    return 1;
  }

  // Read current bake.toml
  Path toml_path = *root / "bake.toml";
  auto content = read_file(toml_path);
  if (!content) {
    std::println(std::cerr, "bake: cannot read bake.toml");
    return 1;
  }

  // Check for duplicate dependency name by parsing the TOML properly.
  // This handles both "name = { ... }" (compact) and "name = {url=...}" forms.
  {
    try {
      auto tbl = toml::parse_file(toml_path.string());
      if (auto *deps = tbl["dependencies"].as_table()) {
        if (deps->contains(name)) {
          std::println(std::cerr,
                       "bake: dependency '{}' already exists in bake.toml",
                       name);
          return 1;
        }
      }
    } catch (...) {
      // If TOML parsing fails, fall through — the write will still work
    }
  }

  // Append dependency to [dependencies] section
  std::string dep_line =
      name + " = { url = \"" + url + "\", tag = \"" + tag + "\" }\n";

  // Check if [dependencies] section exists
  if (contains(*content, "[dependencies]")) {
    // Insert after the [dependencies] line
    std::size_t pos = content->find("[dependencies]");
    pos = content->find('\n', pos);
    if (pos == std::string::npos)
      pos = content->size();
    content->insert(pos + 1, dep_line);
  } else {
    // Add new [dependencies] section at end
    if (!content->empty() && content->back() != '\n')
      *content += "\n";
    *content += "\n[dependencies]\n" + dep_line;
  }

  if (!write_file(toml_path, *content)) {
    std::println(std::cerr, "bake: failed to write bake.toml");
    return 1;
  }

  std::println("Added dependency '{}' = {{ url = \"{}\", tag = \"{}\" }}", name,
               url, tag);
  std::println("Run 'bake build' to resolve and download.");
  return 0;
}

// ===== update command =====

int cmd_update(const ParsedArgs &args) {
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

  if (manifest->dependencies.empty()) {
    std::println("bake: no dependencies to update");
    return 0;
  }

  // Determine which dep to update (empty = all)
  std::string filter_dep;
  if (!args.positional.empty()) {
    filter_dep = args.positional[0];
    if (manifest->dependencies.find(filter_dep) ==
        manifest->dependencies.end()) {
      std::println(std::cerr, "bake: dependency '{}' not found in bake.toml",
                   filter_dep);
      return 1;
    }
    auto &dep = manifest->dependencies[filter_dep];
    if (dep.is_path_dep) {
      std::println(std::cerr,
                   "bake: '{}' is a path dependency (no resolution needed)",
                   filter_dep);
      return 1;
    }
  }

  Path lock_path = *root / "bake.lock";

  // Load existing lock for comparison
  auto old_lock = Lockfile::load(lock_path);

  // Full re-resolve: the lock is flat, so we always resolve the full closure.
  Resolver resolver;
  auto new_lock = resolver.resolve(*manifest, ResolverConfig{});
  if (!new_lock) {
    std::println(std::cerr, "bake: failed to resolve dependencies");
    return 1;
  }

  // Report changes for the filtered dep (or all if no filter)
  auto report_change = [&](const std::string &name) {
    // Find old and new lock entries by URL+ref match
    const auto &dep = manifest->dependencies.at(name);
    const LockDep *old_entry =
        old_lock ? old_lock->find_remote(dep.url, dep.tag) : nullptr;
    const LockDep *new_entry = new_lock->find_remote(dep.url, dep.tag);

    if (!old_entry && new_entry) {
      std::println("bake: {} added", name);
    } else if (old_entry && new_entry) {
      if (old_entry->commit != new_entry->commit) {
        std::println("bake: {} updated: {} → {}", name,
                     old_entry->commit.substr(0, 12),
                     new_entry->commit.substr(0, 12));
      } else {
        std::println("bake: {} unchanged ({})", name,
                     new_entry->commit.substr(0, 12));
      }
    }
  };

  if (filter_dep.empty()) {
    for (auto &[name, dep] : manifest->dependencies) {
      if (dep.is_path_dep)
        continue;
      report_change(name);
    }
  } else {
    report_change(filter_dep);
  }

  if (!new_lock->save(lock_path)) {
    std::println(std::cerr, "bake: failed to write bake.lock");
    return 1;
  }

  std::println("bake: lock file updated");
  return 0;
}

// ===== test command =====
//
// Orchestration layer: builds the project, then runs the test binaries that
// build.cpp registered via b.add_test(). The tests' "testness" lives inside
// the binaries; bake only discovers, builds, runs, and reports.

int cmd_test(const ParsedArgs &args) {
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
  policy.locked = args.has_option("locked") || args.has_option("frozen");
  policy.offline = args.has_option("offline") || args.has_option("frozen");
  auto outer_graph = resolve_moid_graph(*manifest, selection, policy);
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

// ===== Main dispatch =====

int run(int argc, char *argv[]) {
  // Fast path: -cc1 / -cc1as are Clang driver internal modes spawned when
  // the driver falls back to subprocess execution.  These must bypass bake's
  // argument parser entirely (the args contain options like -triple that
  // look like flags to our parser).
  if (argc >= 2 && argv[1]) {
    std::string_view a1(argv[1]);
    if (a1 == "-cc1" || a1 == "-cc1as") {
      return bake_clang_main(argc, const_cast<const char **>(argv));
    }
  }

  ParsedArgs args = parse_args(argc, argv);

  // No command: handle --version / --help / default
  if (args.command.empty()) {
    if (args.version) {
      print_version();
      return 0;
    }
    print_help();
    return 0;
  }

  // Command-specific --help
  if (args.help) {
    print_command_help(args.command);
    return 0;
  }

  // Dispatch
  if (args.command == "init")
    return cmd_init(args);
  if (args.command == "build")
    return cmd_build(args);
  if (args.command == "add")
    return cmd_add(args);
  if (args.command == "update")
    return cmd_update(args);
  if (args.command == "run")
    return cmd_run(args);
  if (args.command == "test")
    return cmd_test(args);
  if (args.command == "clean")
    return cmd_clean(args);
  if (args.command == "audit")
    return cmd_audit(args);

  // Pass raw argv to the embedded Clang driver.
  if (args.command == "cc" || args.command == "c++") {
    return bake_clang_main(argc, const_cast<const char **>(argv));
  }

  // In-process archive writer (replaces system ar).
  // Usage: bake ar rcs [--darwin] <archive> <members...>
  if (args.command == "ar") {
    bool is_darwin = args.has_option("darwin");
    // Skip "rcs" flag (bake ar always creates static archives).
    auto &pos = args.positional;
    std::size_t start = (!pos.empty() && pos[0] == "rcs") ? 1 : 0;
    if (start >= pos.size()) {
      std::println(std::cerr, "bake ar: missing archive name");
      return 1;
    }
    std::string archive_name = pos[start];
    std::vector<std::string> members(pos.begin() + start + 1, pos.end());
    std::vector<const char *> member_ptrs;
    member_ptrs.reserve(members.size());
    for (auto &m : members)
      member_ptrs.push_back(m.c_str());
    int kind = is_darwin ? 2 : 0; // 0=GNU, 2=DARWIN
    return bake_ar_write(archive_name.c_str(), member_ptrs.data(),
                         member_ptrs.size(), kind);
  }

  std::println(std::cerr, "bake: unknown command '{}'", args.command);
  std::println(std::cerr, "Run 'bake --help' for usage.");
  return 1;
}

} // namespace bake::cli

int main(int argc, char* argv[]) {
    return bake::cli::run(argc, argv);
}
