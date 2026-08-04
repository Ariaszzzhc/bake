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
import bake.compiler;
import bake.engine;
import bake.package;

// ============================================================
// bake.cli — multicall dispatch, argument parsing, commands
// ============================================================

#ifndef BAKE_VERSION
#define BAKE_VERSION "0.1.0"
#endif

#ifndef BAKE_SRC_DIR
#define BAKE_SRC_DIR "."
#endif

#ifndef BAKE_LIB_DIR
#define BAKE_LIB_DIR "."
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

    // Options that consume the next argument as a value
    auto takes_value = [](std::string_view opt) {
        return opt == "--tag" || opt == "--type" || opt == "--std" ||
               opt == "--target" || opt == "--glibc-version" ||
               opt == "--option" || opt == "-j" || opt == "-p";
    };

    int i = 1;
    while (i < argc) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            args.help = true;
        } else if (arg == "--version" || arg == "-V") {
            args.version = true;
        } else if (starts_with(arg, "--") || (arg.size() > 1 && arg[0] == '-' && arg[1] != '\0')) {
            // It's an option
            if (contains(arg, "=")) {
                args.options.push_back(arg);
            } else if (takes_value(arg) && i + 1 < argc) {
                args.options.push_back(arg + "=" + argv[i + 1]);
                ++i;
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

// ===== build.cpp compilation + execution =====

export int build_with_build_cpp(const Path& root, const ParsedArgs& args) {
    auto tc = Toolchain::detect();
    Path bake_dir = root / ".bake";
    bake_dir.mkdir_recursive();

    Path build_cpp = root / "build.cpp";
    Path wrapper_src = Path(BAKE_SRC_DIR) / "libbake" / "public" / "bake.build.cppm";
    Path cabi_header = Path(BAKE_SRC_DIR) / "libbake" / "src" / "cabi" / "bake_cabi.h";

    // Copy wrapper + header to .bake/
    Path wrapper_dst = bake_dir / "bake.build.cppm";
    Path cabi_dst = bake_dir / "bake_cabi.h";
    auto wrapper_content = read_file(wrapper_src);
    auto cabi_content = read_file(cabi_header);
    if (!wrapper_content || !cabi_content) {
        std::fprintf(stderr, "bake: cannot find bake.build.cppm or bake_cabi.h\n");
        return 1;
    }
    write_file(wrapper_dst, *wrapper_content);
    write_file(cabi_dst, *cabi_content);

    // Step 1: Compile wrapper module → BMI + .o
    Path pcm = bake_dir / "bake.build.pcm";
    Path wrapper_o = bake_dir / "bake.build.o";

    std::printf("bake: compiling build script...\n");

    {
        std::vector<std::string> cmd;
        cmd.push_back(tc.cxx_path);
        cmd.push_back("-c");
        cmd.push_back("-std=c++23");
        cmd.push_back("-x");
        cmd.push_back("c++-module");
        cmd.push_back("-I" + bake_dir.string());
        cmd.push_back("-fmodule-output=" + pcm.string());
        cmd.push_back(wrapper_dst.string());
        cmd.push_back("-o");
        cmd.push_back(wrapper_o.string());

        auto result = run_process(cmd, root, true);
        if (!result.success()) {
            std::fprintf(stderr, "%s", result.stderr_output.c_str());
            std::fprintf(stderr, "bake: failed to compile bake.build.cppm\n");
            return 1;
        }
    }

    // Step 2: Compile build.cpp → .o
    Path build_o = bake_dir / "build.o";
    {
        std::vector<std::string> cmd;
        cmd.push_back(tc.cxx_path);
        cmd.push_back("-c");
        cmd.push_back("-std=c++23");
        cmd.push_back("-I" + bake_dir.string());
        if (tc.is_clang()) {
            cmd.push_back("-fmodule-file=bake.build=" + pcm.string());
        }
        cmd.push_back(build_cpp.string());
        cmd.push_back("-o");
        cmd.push_back(build_o.string());

        auto result = run_process(cmd, root, true);
        if (!result.success()) {
            std::fprintf(stderr, "%s", result.stderr_output.c_str());
            std::fprintf(stderr, "bake: failed to compile build.cpp\n");
            return 1;
        }
    }

    // Step 3: Link → build_app
    Path build_app = bake_dir / "build_app";
    {
        std::vector<std::string> cmd;
        cmd.push_back(tc.cxx_path);
        cmd.push_back(wrapper_o.string());
        cmd.push_back(build_o.string());
        cmd.push_back("-L" + std::string(BAKE_LIB_DIR));
        cmd.push_back("-lbake");
        // RPATH so build_app can find libbake at runtime
        cmd.push_back("-Wl,-rpath," + std::string(BAKE_LIB_DIR));
        cmd.push_back("-o");
        cmd.push_back(build_app.string());

        auto result = run_process(cmd, root, true);
        if (!result.success()) {
            std::fprintf(stderr, "%s", result.stderr_output.c_str());
            std::fprintf(stderr, "bake: failed to link build_app\n");
            return 1;
        }
    }

    // Step 4: Run build_app → .bake/build.json
    {
        auto result = run_process({build_app.string()}, root, true);
        if (!result.success()) {
            std::fprintf(stderr, "%s", result.stderr_output.c_str());
            std::fprintf(stderr, "bake: build_app failed\n");
            return 1;
        }
    }

    // Step 5: Read build.json → execute
    Path build_json = bake_dir / "build.json";
    if (!build_json.is_regular_file()) {
        std::fprintf(stderr, "bake: build_app did not produce .bake/build.json\n");
        return 1;
    }

    auto plan = read_build_json(build_json, root);

    // Write compile_commands.json
    write_compile_commands(plan, root / "compile_commands.json");

    // Get job count
    int jobs = 0;
    if (auto j = args.get_option("j")) {
        jobs = std::atoi(j->c_str());
    }

    return execute_plan(plan, jobs);
}

// ===== build command =====

export int cmd_build(const ParsedArgs& args) {
    // Find project root
    auto root = find_project_root();
    if (!root) {
        std::fprintf(stderr, "bake: no bake.toml found in current directory or any parent\n");
        return 1;
    }

    // Check for build.cpp (escape hatch mode)
    Path build_cpp = *root / "build.cpp";
    if (build_cpp.is_regular_file()) {
        return build_with_build_cpp(*root, args);
    }

    // Load manifest
    auto manifest = Manifest::load(*root);
    if (!manifest) {
        std::fprintf(stderr, "bake: failed to load bake.toml\n");
        return 1;
    }

    // ===== Lock resolution =====
    bool offline = args.has_option("offline") || args.has_option("frozen");
    bool locked = args.has_option("locked") || args.has_option("frozen");

    if (!manifest->dependencies.empty()) {
        Path lock_path = *root / "bake.lock";
        auto lockfile = Lockfile::load(lock_path);

        bool needs_resolve = false;
        if (!lockfile) {
            needs_resolve = true;
        } else if (!lockfile->is_consistent(*manifest)) {
            needs_resolve = true;
        }

        if (needs_resolve) {
            if (locked) {
                std::fprintf(stderr, "bake: lock file missing or stale (--locked)\n");
                return 1;
            }
            if (offline) {
                std::fprintf(stderr, "bake: cannot resolve dependencies in offline mode\n");
                return 1;
            }
            // Resolve dependencies
            Resolver resolver;
            auto new_lock = resolver.resolve(*manifest, ResolverConfig{});
            if (!new_lock) {
                std::fprintf(stderr, "bake: failed to resolve dependencies\n");
                return 1;
            }
            if (!new_lock->save(lock_path)) {
                std::fprintf(stderr, "bake: failed to write bake.lock\n");
                return 1;
            }
            std::printf("bake: dependencies resolved and locked\n");
        }
    }
    // ===== End lock resolution =====

    // Handle workspace: build all members
    if (manifest->is_workspace()) {
        int result = 0;
        for (auto& member : manifest->workspace->members) {
            Path member_dir = *root / member;
            auto member_manifest = Manifest::load(member_dir);
            if (!member_manifest || !member_manifest->has_package()) {
                std::fprintf(stderr, "bake: skipping workspace member '%s' (no [package])\n", member.c_str());
                continue;
            }

            // Check -p flag
            if (auto p = args.get_option("p")) {
                if (*p != member_manifest->package->name) continue;
            }

            std::printf("bake: building '%s'\n", member_manifest->package->name.c_str());
            auto layout = Layout::detect(member_dir);
            auto tc = Toolchain::detect();

            // Apply option overrides
            auto options = member_manifest->options;
            for (auto& opt : args.options) {
                if (starts_with(opt, "--option=") || opt == "--option") {
                    // Parse --option name=value or --option=name=value
                    std::string val = starts_with(opt, "--option=") ? opt.substr(9) : "";
                    if (val.empty()) continue;
                    size_t eq = val.find('=');
                    std::string name = (eq == std::string::npos) ? val : val.substr(0, eq);
                    std::string value = (eq == std::string::npos) ? "" : val.substr(eq + 1);
                    if (!value.empty()) {
                        options[name] = BuildOption::from_string(value);
                    } else {
                        options[name] = BuildOption::from_bool(true);
                    }
                }
            }

            auto plan = create_convention_plan(*member_manifest, layout, tc, options);

            // Get job count
            int jobs = 0;
            if (auto j = args.get_option("j")) {
                jobs = std::atoi(j->c_str());
            }

            int r = execute_plan(plan, jobs);
            if (r != 0) result = r;
        }
        return result;
    }

    if (!manifest->has_package()) {
        std::fprintf(stderr, "bake: no [package] section in bake.toml\n");
        return 1;
    }

    // Single package build
    auto layout = Layout::detect(*root);
    auto tc = Toolchain::detect();

    std::printf("bake: detected %s (%s)\n", tc.kind_name().c_str(), tc.cxx_path.c_str());

    // Apply option overrides
    auto options = manifest->options;
    for (auto& opt : args.options) {
        if (starts_with(opt, "--option=") || opt == "--option") {
            std::string val = starts_with(opt, "--option=") ? opt.substr(9) : "";
            if (val.empty()) continue;
            size_t eq = val.find('=');
            std::string name = (eq == std::string::npos) ? val : val.substr(0, eq);
            std::string value = (eq == std::string::npos) ? "" : val.substr(eq + 1);
            if (!value.empty()) {
                options[name] = BuildOption::from_string(value);
            } else {
                options[name] = BuildOption::from_bool(true);
            }
        }
    }

    auto plan = create_convention_plan(*manifest, layout, tc, options);

    // Write compile_commands.json
    write_compile_commands(plan, *root / "compile_commands.json");

    // Get job count
    int jobs = 0;
    if (auto j = args.get_option("j")) {
        jobs = std::atoi(j->c_str());
    }

    return execute_plan(plan, jobs);
}

// ===== clean command =====

export int cmd_clean(const ParsedArgs& args) {
    auto root = find_project_root();
    if (!root) {
        std::fprintf(stderr, "bake: no bake.toml found\n");
        return 1;
    }

    auto layout = Layout::detect(*root);

    int removed = 0;
    if (layout.build_dir.exists()) {
        layout.build_dir.remove_all();
        std::printf("Removed %s\n", layout.build_dir.string().c_str());
        removed++;
    }
    if (layout.artifacts_dir.exists()) {
        layout.artifacts_dir.remove_all();
        std::printf("Removed %s\n", layout.artifacts_dir.string().c_str());
        removed++;
    }

    // Clean workspace members
    auto manifest = Manifest::load(*root);
    if (manifest && manifest->is_workspace()) {
        for (auto& member : manifest->workspace->members) {
            Path member_dir = *root / member;
            auto ml = Layout::detect(member_dir);
            if (ml.build_dir.exists()) {
                ml.build_dir.remove_all();
                std::printf("Removed %s\n", ml.build_dir.string().c_str());
                removed++;
            }
            if (ml.artifacts_dir.exists()) {
                ml.artifacts_dir.remove_all();
                std::printf("Removed %s\n", ml.artifacts_dir.string().c_str());
                removed++;
            }
        }
    }

    if (removed == 0) {
        std::printf("Nothing to clean\n");
    }

    return 0;
}

// ===== run command =====

export int cmd_run(const ParsedArgs& args) {
    // First build
    int build_result = cmd_build(args);
    if (build_result != 0) return build_result;

    // Find the built executable
    auto root = find_project_root();
    if (!root) return 1;

    Path exe_path;

    // If build.cpp was used, read primary output from build.json
    Path build_json = *root / ".bake" / "build.json";
    Path build_cpp = *root / "build.cpp";
    if (build_cpp.is_regular_file() && build_json.is_regular_file()) {
        auto plan = read_build_json(build_json, *root);
        if (plan.primary_output.string() != "") {
            exe_path = *root / plan.primary_output;
        }
    }

    // Convention mode fallback
    if (exe_path.string() == "") {
        auto manifest = Manifest::load(*root);
        if (!manifest || !manifest->has_package()) return 1;

        if (manifest->package->type != PackageType::Executable) {
            std::fprintf(stderr, "bake: cannot run non-executable package\n");
            return 1;
        }

        auto layout = Layout::detect(*root);
        std::string exe_name = library_name(manifest->package->name, PackageType::Executable);
        exe_path = layout.artifacts_dir / exe_name;
    }

    if (!exe_path.exists()) {
        std::fprintf(stderr, "bake: built executable not found at %s\n", exe_path.string().c_str());
        return 1;
    }

    // Build command line: executable + remaining args
    std::vector<std::string> run_args;
    for (auto& p : args.positional) {
        run_args.push_back(p);
    }

    auto result = run_process(exe_path.string(), run_args, *root);
    return result.exit_code;
}

// ===== add command =====

export int cmd_add(const ParsedArgs& args) {
    auto root = find_project_root();
    if (!root) {
        std::fprintf(stderr, "bake: no bake.toml found\n");
        return 1;
    }

    if (args.positional.empty()) {
        std::fprintf(stderr, "bake: add requires a URL\n");
        std::fprintf(stderr, "Usage: bake add <url> --tag <tag> [name]\n");
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
        size_t last_slash = url.find_last_of('/');
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
        std::fprintf(stderr, "bake: add requires --tag <tag>\n");
        return 1;
    }

    // Read current bake.toml
    Path toml_path = *root / "bake.toml";
    auto content = read_file(toml_path);
    if (!content) {
        std::fprintf(stderr, "bake: cannot read bake.toml\n");
        return 1;
    }

    // Append dependency to [dependencies] section
    std::string dep_line = name + " = { url = \"" + url + "\", tag = \"" + tag + "\" }\n";

    // Check if [dependencies] section exists
    if (contains(*content, "[dependencies]")) {
        // Insert after the [dependencies] line
        size_t pos = content->find("[dependencies]");
        pos = content->find('\n', pos);
        if (pos == std::string::npos) pos = content->size();
        content->insert(pos + 1, dep_line);
    } else {
        // Add new [dependencies] section at end
        if (!content->empty() && content->back() != '\n') *content += "\n";
        *content += "\n[dependencies]\n" + dep_line;
    }

    if (!write_file(toml_path, *content)) {
        std::fprintf(stderr, "bake: failed to write bake.toml\n");
        return 1;
    }

    std::printf("Added dependency '%s' = { url = \"%s\", tag = \"%s\" }\n",
                name.c_str(), url.c_str(), tag.c_str());
    std::printf("Run 'bake build' to resolve and download.\n");
    return 0;
}

// ===== update command =====

export int cmd_update(const ParsedArgs& args) {
    auto root = find_project_root();
    if (!root) {
        std::fprintf(stderr, "bake: no bake.toml found\n");
        return 1;
    }

    auto manifest = Manifest::load(*root);
    if (!manifest) {
        std::fprintf(stderr, "bake: failed to load bake.toml\n");
        return 1;
    }

    if (manifest->dependencies.empty()) {
        std::printf("bake: no dependencies to update\n");
        return 0;
    }

    Path lock_path = *root / "bake.lock";

    // Load existing lock for comparison
    auto old_lock = Lockfile::load(lock_path);

    // Re-resolve
    Resolver resolver;
    auto new_lock = resolver.resolve(*manifest, ResolverConfig{});

    if (!new_lock) {
        std::fprintf(stderr, "bake: failed to resolve dependencies\n");
        return 1;
    }

    // Compare with old lock
    if (old_lock) {
        for (auto& [name, node_id] : new_lock->root_deps) {
            auto old_it = old_lock->root_deps.find(name);
            if (old_it != old_lock->root_deps.end()) {
                auto& old_node = old_lock->nodes[old_it->second];
                auto& new_node = new_lock->nodes[node_id];
                if (old_node.commit != new_node.commit) {
                    std::printf("bake: %s updated: %s → %s\n",
                                name.c_str(), old_node.commit.substr(0, 12).c_str(),
                                new_node.commit.substr(0, 12).c_str());
                }
            } else {
                std::printf("bake: %s added\n", name.c_str());
            }
        }
    }

    // Write lock file
    if (!new_lock->save(lock_path)) {
        std::fprintf(stderr, "bake: failed to write bake.lock\n");
        return 1;
    }

    std::printf("bake: lock file updated\n");
    return 0;
}

// ===== Stub commands =====

export int cmd_test(const ParsedArgs& args) {
    std::fprintf(stderr, "bake: test not yet implemented (Phase 7)\n");
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
