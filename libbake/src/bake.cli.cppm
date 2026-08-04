module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <cstdio>
#include <cstdlib>

export module bake.cli;

import bake.util;
import bake.project;

// ============================================================
// bake.cli — multicall dispatch, argument parsing, commands
// ============================================================

#ifndef BAKE_VERSION
#define BAKE_VERSION "0.1.0"
#endif

namespace bake::cli {

// ===== Argument parsing =====

export struct ParsedArgs {
    std::string command;                // subcommand (empty if none)
    std::vector<std::string> positional; // positional arguments
    std::vector<std::string> options;    // --flag or --key=value
    bool help = false;
    bool version = false;

    // Get an option value (--key=value or --key value)
    std::optional<std::string> get_option(std::string_view name) const {
        std::string prefix = std::string("--") + std::string(name) + "=";
        for (auto& opt : options) {
            if (opt == prefix.substr(0, prefix.size() - 1)) {
                return std::string("true");  // flag without value
            }
            if (starts_with(opt, prefix)) {
                return opt.substr(prefix.size());
            }
        }
        return std::nullopt;
    }

    bool has_option(std::string_view name) const {
        std::string flag = std::string("--") + std::string(name);
        std::string prefix = flag + "=";
        for (auto& opt : options) {
            if (opt == flag) return true;
            if (starts_with(opt, prefix)) return true;
        }
        return false;
    }
};

export ParsedArgs parse_args(int argc, char* argv[]) {
    ParsedArgs args;

    int i = 1;
    // First positional or --help/--version is the command
    while (i < argc) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            args.help = true;
        } else if (arg == "--version" || arg == "-V") {
            args.version = true;
        } else if (starts_with(arg, "--")) {
            args.options.push_back(arg);
        } else if (args.command.empty()) {
            args.command = arg;
        } else {
            args.positional.push_back(arg);
        }
        ++i;
    }

    return args;
}

// ===== Help text =====

export void print_version() {
    std::printf("bake %s\n", BAKE_VERSION);
}

export void print_help() {
    std::printf(
        "bake %s — All-in-One C/C++ toolchain\n"
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
        "\n"
        "GLOBAL OPTIONS:\n"
        "    -V, --version   Print version and exit\n"
        "    -h, --help      Print this help and exit\n"
        "\n"
        "BUILD OPTIONS:\n"
        "    --option <name>[=value]  Override a [options] value from bake.toml\n"
        "    --target=<triple>       Cross-compile target\n"
        "    --locked                Fail if lock is missing or stale\n"
        "    --offline               Never connect to the network\n"
        "    --frozen                Equivalent to --locked --offline\n"
        "    -p <member>             Build a specific workspace member\n"
        "    -j <n>                  Parallel job count\n"
        "\n"
        "For more information, visit: https://github.com/arias/bake\n",
        BAKE_VERSION
    );
}

export void print_command_help(std::string_view cmd) {
    if (cmd == "init") {
        std::printf(
            "bake init — Create a new project scaffold\n"
            "\n"
            "USAGE:\n"
            "    bake init [name] [options]\n"
            "\n"
            "OPTIONS:\n"
            "    --type <executable|static-lib|shared-lib>  Package type (default: executable)\n"
            "    --std <c++17|c++20|c++23>                  C++ standard (default: c++20)\n"
            "\n"
            "If [name] is omitted, scaffolds in the current directory.\n"
        );
    } else if (cmd == "build") {
        std::printf(
            "bake build — Build the project\n"
            "\n"
            "USAGE:\n"
            "    bake build [options]\n"
            "\n"
            "OPTIONS:\n"
            "    --option <name>[=value]  Override a [options] value\n"
            "    -j <n>                   Parallel job count\n"
            "    -p <member>              Build specific workspace member\n"
            "    --locked                 Fail if lock is missing/stale\n"
            "    --offline                No network access\n"
            "    --frozen                 --locked + --offline\n"
        );
    } else if (cmd == "clean") {
        std::printf(
            "bake clean — Remove build artifacts\n"
            "\n"
            "USAGE:\n"
            "    bake clean\n"
        );
    } else {
        print_help();
    }
}

// ===== init command =====

export int cmd_init(const ParsedArgs& args) {
    // Determine project name
    std::string project_name;
    if (!args.positional.empty()) {
        project_name = args.positional[0];
    } else {
        // Use current directory name
        auto cwd = Path::current();
        project_name = cwd.filename_string();
        if (project_name.empty() || project_name == "/") {
            std::fprintf(stderr, "bake: could not determine project name. Please specify: bake init <name>\n");
            return 1;
        }
    }

    // Determine package type
    PackageType pkg_type = PackageType::Executable;
    if (auto type_str = args.get_option("type")) {
        if (auto t = parse_package_type(*type_str)) {
            pkg_type = *t;
        } else {
            std::fprintf(stderr, "bake: unknown package type '%s'\n", type_str->c_str());
            return 1;
        }
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
            std::fprintf(stderr, "bake: directory '%s' already exists\n", project_name.c_str());
            return 1;
        }
    } else {
        target_dir = Path::current();
    }

    // Create directories
    (target_dir / "src").mkdir_recursive();
    (target_dir / "public").mkdir_recursive();
    (target_dir / "tests").mkdir_recursive();

    // Write bake.toml
    std::string toml_content = std::string("[package]\n")
        + "name = \"" + project_name + "\"\n"
        + "version = \"0.1.0\"\n"
        + "type = \"" + std::string(package_type_str(pkg_type)) + "\"\n"
        + "std = \"" + std_ver + "\"\n\n";

    write_file(target_dir / "bake.toml", toml_content);

    // Write source file
    if (pkg_type == PackageType::Executable) {
        std::string main_cpp = std::string(
            "#include <iostream>\n\n"
            "int main() {\n"
            "    std::cout << \"Hello from " + project_name + "!\\n\";\n"
            "    return 0;\n"
            "}\n"
        );
        write_file(target_dir / "src" / "main.cpp", main_cpp);
    } else {
        std::string lib_cpp = std::string(
            "// " + project_name + " library\n\n"
            "// Define your library functions here\n"
        );
        std::string filename = (pkg_type == PackageType::StaticLib) ? "lib.cpp" : "lib.cpp";
        write_file(target_dir / "src" / filename, lib_cpp);

        // Public header
        std::string public_dir = "public/" + project_name;
        (target_dir / public_dir).mkdir_recursive();
        std::string header = std::string(
            "#pragma once\n\n"
            "// " + project_name + " public API\n"
        );
        write_file(target_dir / public_dir / (project_name + ".hpp"), header);
    }

    // Write .gitignore
    write_file(target_dir / ".gitignore",
        "/.bake/\n"
        "/build/\n"
        "/artifacts/\n"
        "compile_commands.json\n"
    );

    // Print success
    if (create_subdir) {
        std::printf("Created project '%s' in ./%s/\n", project_name.c_str(), project_name.c_str());
        std::printf("  Next: cd %s && bake build\n", project_name.c_str());
    } else {
        std::printf("Initialized project '%s' in current directory\n", project_name.c_str());
        std::printf("  Next: bake build\n");
    }

    return 0;
}

// ===== Stub commands (Phase 1+ implementations) =====

export int cmd_build(const ParsedArgs& args) {
    std::fprintf(stderr, "bake: build not yet implemented (Phase 1)\n");
    return 1;
}

export int cmd_add(const ParsedArgs& args) {
    std::fprintf(stderr, "bake: add not yet implemented (Phase 3)\n");
    return 1;
}

export int cmd_update(const ParsedArgs& args) {
    std::fprintf(stderr, "bake: update not yet implemented (Phase 3)\n");
    return 1;
}

export int cmd_run(const ParsedArgs& args) {
    std::fprintf(stderr, "bake: run not yet implemented (Phase 1)\n");
    return 1;
}

export int cmd_test(const ParsedArgs& args) {
    std::fprintf(stderr, "bake: test not yet implemented (Phase 7)\n");
    return 1;
}

export int cmd_clean(const ParsedArgs& args) {
    std::fprintf(stderr, "bake: clean not yet implemented (Phase 1)\n");
    return 1;
}

// ===== Main dispatch =====

export int main(int argc, char* argv[]) {
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
    if (args.command == "init")     return cmd_init(args);
    if (args.command == "build")    return cmd_build(args);
    if (args.command == "add")      return cmd_add(args);
    if (args.command == "update")   return cmd_update(args);
    if (args.command == "run")      return cmd_run(args);
    if (args.command == "test")     return cmd_test(args);
    if (args.command == "clean")    return cmd_clean(args);

    // Version / help as commands
    if (args.command == "--version" || args.command == "-V") {
        print_version();
        return 0;
    }
    if (args.command == "--help" || args.command == "-h") {
        print_help();
        return 0;
    }

    std::fprintf(stderr, "bake: unknown command '%s'\n", args.command.c_str());
    std::fprintf(stderr, "Run 'bake --help' for usage.\n");
    return 1;
}

} // namespace bake::cli
