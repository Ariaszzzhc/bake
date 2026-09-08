import std;
import bake.stage0.cli;
import bake.stage0.build;
import bake.stage0.update;
import bake.toolchain.driver;
import bake.toolchain.lld;

namespace bake::cli {

int run(int argc, char *argv[]) {
  // -cc1 / -cc1as: raw Clang driver internals spawned during builds.
  if (argc >= 2 && argv[1]) {
    std::string_view a1(argv[1]);
    if (a1 == "-cc1" || a1 == "-cc1as")
      return bake_clang_main(argc, const_cast<const char **>(argv));
  }

  ParsedArgs args = parse_args(argc, argv);
  if (args.command.empty()) {
    if (args.version) {
      print_version();
      return 0;
    }
    std::println("bake (stage0): bootstrap binary\n"
                 "usage: bake <build|update|cc|c++|ar>");
    return args.help ? 0 : 1;
  }

  if (args.command == "build") return cmd_build(args);
  if (args.command == "update") return cmd_update(args);

  if (args.command == "cc" || args.command == "c++")
    return bake_clang_main(argc, const_cast<const char **>(argv));

  if (args.command == "ar") {
    bool is_darwin = args.has_option("darwin");
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

  std::println(std::cerr,
               "bake (stage0): command '{}' is not part of the bootstrap "
               "binary",
               args.command);
  return 1;
}

} // namespace bake::cli

int main(int argc, char *argv[]) { return bake::cli::run(argc, argv); }
