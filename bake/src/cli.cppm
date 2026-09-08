// bake.cli — argument parsing and help text shared by every command
// implementation (bake.cli.* modules).
// The composition roots (src/main.cpp, src/stage0/main.cpp) own the
// dispatch tables; this module owns everything the commands share.

export module bake.cli;

import std;
import bake.util;
import bake.buildsystem.project;

#ifndef BAKE_VERSION
#define BAKE_VERSION "0.1.0"
#endif

namespace bake::cli {

// ===== Argument parsing =====

export struct ParsedArgs {
  std::string command;                 // subcommand (empty if none)
  std::vector<std::string> positional; // positional arguments
  std::vector<std::string> options;    // --flag or --key=value
  // Every argv token except the subcommand, preserved in its original order
  // for command-specific parsers such as the LSP entry point.
  std::vector<std::string> raw_arguments;
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

export ParsedArgs parse_args(int argc, char *argv[]) {
  ParsedArgs args;

  // Options that consume the next argument as a value
  auto takes_value = [](std::string_view opt) {
    return opt == "--tag" || opt == "--type" || opt == "--std" ||
           opt == "--target" || opt == "--glibc-version" || opt == "--feature" ||
           opt == "--profile" || opt == "--index-jobs" || opt == "-j" ||
           opt == "-p" || opt == "-t";
  };

  int i = 1;
  while (i < argc) {
    std::string arg = argv[i];

    if (arg == "--help" || arg == "-h") {
      args.help = true;
      args.raw_arguments.push_back(arg);
    } else if (arg == "--version" || arg == "-V") {
      args.version = true;
      args.raw_arguments.push_back(arg);
    } else if (starts_with(arg, "--") ||
               (arg.size() > 1 && arg[0] == '-' && arg[1] != '\0')) {
      // It's an option
      if (contains(arg, "=")) {
        args.options.push_back(arg);
        args.raw_arguments.push_back(arg);
      } else if (takes_value(arg)) {
        if (i + 1 < argc) {
          args.options.push_back(arg + "=" + argv[i + 1]);
          args.raw_arguments.push_back(arg);
          args.raw_arguments.push_back(argv[i + 1]);
          ++i;
        } else {
          // Missing value — push as empty to trigger validation errors
          args.options.push_back(std::string(arg) + "=");
          args.raw_arguments.push_back(arg);
        }
      } else {
        args.options.push_back(arg);
        args.raw_arguments.push_back(arg);
      }
    } else if (args.command.empty()) {
      args.command = arg;
    } else {
      args.positional.push_back(arg);
      args.raw_arguments.push_back(arg);
    }
    ++i;
  }

  return args;
}

// ===== Shared feature-flag helpers =====

export std::optional<std::vector<std::string>>
parse_requested_features(const ParsedArgs &args) {
  std::vector<std::string> names;
  for (const auto &option : args.options) {
    if (!starts_with(option, "--feature="))
      continue;

    const std::string name = option.substr(10);
    if (name.empty()) {
      std::println(std::cerr,
                   "bake: --feature requires a feature name");
      return std::nullopt;
    }
    names.push_back(name);
  }
  return names;
}

export std::optional<std::vector<std::string>>
parse_root_features(const Manifest &root, const ParsedArgs &args,
                    const std::optional<std::string> &selected_member) {
  auto names = parse_requested_features(args);
  if (!names)
    return std::nullopt;
  if (names->empty())
    return std::vector<std::string>{};

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
        "bake: cannot apply features without a selected root moid");
    return std::nullopt;
  }

  for (const auto &name : *names) {
    if (!selected_roots.front().features.count(name)) {
      std::println(std::cerr,
                   "bake: unknown feature '{}' (declare it in [features])",
                   name);
      return std::nullopt;
    }
  }
  return names;
}

// ===== Help text =====

export void print_version() { std::println("bake {}", BAKE_VERSION); }

export std::string bake_version() { return BAKE_VERSION; }

export std::size_t requested_concurrency(const ParsedArgs &args) {
  if (auto jobs = args.get_option("j")) {
    int parsed = std::atoi(jobs->c_str());
    if (parsed > 0)
      return static_cast<std::size_t>(parsed);
  }
  return std::max(1u, std::thread::hardware_concurrency());
}

export void print_help() {
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
        "    remove <name>   Remove a dependency from bake.toml and bake.lock\n"
      "    update [dep]    Re-resolve dependencies and update bake.lock\n"
      "    run             Build and run the executable\n"
      "    test            Build and run tests\n"
      "    clean           Remove build artifacts\n"
      "    cc              Invoke embedded Clang C driver\n"
      "    c++             Invoke embedded Clang C++ driver\n"
      "    ar              Create static archive\n"
      "    audit           Verify toolchain self-containment\n"
      "    compdb          Write compile_commands.json (no build)\n"
      "    lsp             Language server over stdio (stage1)\n"
      "\n"
      "GLOBAL OPTIONS:\n"
      "    -V, --version   Print version and exit\n"
      "    -h, --help      Print this help and exit\n"
      "\n"
      "BUILD OPTIONS:\n"
      "    --feature <name>        Activate a root [features] feature\n"
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
      "Run 'bake <command> --help' for command details.",
      BAKE_VERSION);
}

export void print_command_help(std::string_view cmd) {
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
        "    --feature <name>        Activate a root feature\n"
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

} // namespace bake::cli
