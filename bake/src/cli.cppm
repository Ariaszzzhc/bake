// bake.cli — argument parsing, help text and the lock-enforcement
// shared by every command implementation (bake.cli.* modules).
// The composition roots (src/main.cpp, src/stage0/main.cpp) own the
// dispatch tables; this module owns everything the commands share.

export module bake.cli;

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

#ifndef BAKE_VERSION
#define BAKE_VERSION "0.1.0"
#endif

namespace bake::cli {

// ===== Argument parsing =====

export struct ParsedArgs {
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

export ParsedArgs parse_args(int argc, char *argv[]) {
  ParsedArgs args;

  // Options that consume the next argument as a value
  auto takes_value = [](std::string_view opt) {
    return opt == "--tag" || opt == "--type" || opt == "--std" ||
           opt == "--target" || opt == "--glibc-version" || opt == "--feature" ||
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

// ===== Shared feature-flag helpers =====

namespace {

std::optional<std::vector<std::string>>
parse_feature_flags(const ParsedArgs &args) {
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
} // namespace

export std::optional<std::vector<std::string>>
parse_root_features(const Manifest &root, const ParsedArgs &args,
                    const std::optional<std::string> &selected_member) {
  auto names = parse_feature_flags(args);
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

// The triple used for target-scoped dependency filtering — same
// convention as the engine's target-condition matching: native builds
// carry the detected host triple.
export std::string selection_triple(const TargetSpec &target) {
  return target.is_native() ? detect_host_target().triple_ : target.triple_;
}

// ===== Help text =====

export void print_version() { std::println("bake {}", BAKE_VERSION); }

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

// ===== Lock enforcement =====

// Check and resolve the lockfile before building. Returns 0 if the lock is
// consistent or was successfully (re)resolved. Returns non-zero on failure.
// On success, root/bake.lock is guaranteed to exist and be consistent.
//
// --locked/--frozen: full enforcement — consistency check + cache verification.
// --offline (without --locked): skip — let resolve_moid_graph produce specific
//   ownership diagnostics from the lockfile state.
// Normal: resolve if needed, verify/redownload cache.
export int enforce_lock(const Path &root, const Manifest &manifest,
                 const ParsedArgs &args) {
  if (Manifest::has_only_path_deps(manifest))
    return 0;

  // Lock usage policy, named after the CLI flags:
  //   Update — check consistency; resolve/redownload as needed (default)
  //   Locked — require a fresh lock and intact cache; never write
  //   Frozen — Locked plus a hard network ban (--offline implies the rest)
  enum class LockUsage { Update, Locked, Frozen };
  const LockUsage usage = args.has_option("frozen")   ? LockUsage::Frozen
                          : args.has_option("locked") ? LockUsage::Locked
                                                      : LockUsage::Update;
  const bool offline =
      args.has_option("offline") || usage == LockUsage::Frozen;
  const bool locked = usage != LockUsage::Update;

  Path lock_path = root / "bake.lock";
  auto lockfile = Lockfile::load(lock_path);

  bool needs_resolve = !lockfile || !lockfile->is_consistent(manifest, root);

  // CLI root features change the dependency closure: re-resolve so their
  // conditional dependencies enter the lock (hints carry the rest).
  std::set<std::string> cli_features;
  {
    auto member =
        resolve_workspace_member_selection(manifest, args.get_option("p"));
    if (!member) {
      std::println(std::cerr, "bake: {}", member.error());
      return 1;
    }
    auto names = parse_root_features(manifest, args, *member);
    if (!names)
      return 1;
    cli_features.insert(names->begin(), names->end());
    if (!cli_features.empty())
      needs_resolve = true;
  }

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

  // Normal build, lock missing/stale: resolve dependencies. Entries whose
  // url+ref still match the stale lock are carried over — only new or
  // changed dependencies touch the network.
  Resolver resolver;
  auto new_lock = resolver.resolve(manifest, ResolverConfig{},
                                   lockfile ? &*lockfile : nullptr,
                                   cli_features);
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

} // namespace bake::cli
