// bake — stage1 composition root: the full multicall surface (build
// system commands, toolchain driver, language server). Dispatch only;
// implementations live in bake.cli.* and bake.lsp.

import std;
import bake.util;
import bake.cli;
import bake.cli.dep_edit;
import bake.cli.init;
import bake.cli.build;
import bake.cli.clean;
import bake.cli.audit;
import bake.cli.compdb;
import bake.cli.run;
import bake.cli.add;
import bake.cli.remove;
import bake.cli.update;
import bake.cli.test;
import bake.lsp;
import bake.toolchain.driver;
import bake.toolchain.lld;

namespace bake::cli {

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
  if (args.command == "remove")
    return cmd_remove(args);
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
  if (args.command == "compdb")
    return cmd_compdb(args);
  if (args.command == "lsp")
    return bake::lsp_main(args.positional);
  // Internal: spawned by `bake lsp` for every frontend job.
  if (args.command == "lsp-worker")
    return bake::lsp_worker_main(args.positional);

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

int main(int argc, char *argv[]) { return bake::cli::run(argc, argv); }
