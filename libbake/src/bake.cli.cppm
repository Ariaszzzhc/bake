module;

#include <toml.hpp>

export module bake.cli;

import std;
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

    // Get an option value (--key=value, --key value, -k value, or -k=value)
    std::optional<std::string> get_option(std::string_view name) const {
        std::string long_prefix = std::string("--") + std::string(name) + "=";
        std::string short_prefix = std::string("-") + std::string(name) + "=";
        for (auto& opt : options) {
            if (opt == long_prefix.substr(0, long_prefix.size() - 1) ||
                opt == short_prefix.substr(0, short_prefix.size() - 1)) {
                return std::string("true");  // flag without value
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
        for (auto& opt : options) {
            if (opt == long_flag || opt == short_flag) return true;
            if (starts_with(opt, long_prefix) || starts_with(opt, short_prefix)) return true;
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

// ===== Help text =====

export void print_version() {
    std::println("bake {}", BAKE_VERSION);
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
        "For more information, visit: https://github.com/arias/bake",
        BAKE_VERSION
    );
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
            "    --type <executable|static-lib|shared-lib>  Package type (default: executable)\n"
            "    --std <c++17|c++20|c++23>                  C++ standard (default: c++20)\n"
            "\n"
            "If [name] is omitted, scaffolds in the current directory."
        );
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
            "    --locked                 Fail if lock is missing/stale\n"
            "    --offline                No network access\n"
            "    --frozen                 --locked + --offline"
        );
    } else if (cmd == "clean") {
        std::println(
            "bake clean — Remove build artifacts\n"
            "\n"
            "USAGE:\n"
            "    bake clean"
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
            std::println(std::cerr, "bake: could not determine project name. Please specify: bake init <name>");
            return 1;
        }
    }

    // Determine package type
    PackageType pkg_type = PackageType::Executable;
    if (auto type_str = args.get_option("type")) {
        if (auto t = parse_package_type(*type_str)) {
            pkg_type = *t;
        } else {
            std::println(std::cerr, "bake: unknown package type '{}'", *type_str);
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
            std::println(std::cerr, "bake: directory '{}' already exists", project_name);
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
        std::println("Created project '{}' in ./{}", project_name, project_name);
        std::println("  Next: cd {} && bake build", project_name);
    } else {
        std::println("Initialized project '{}' in current directory", project_name);
        std::println("  Next: bake build");
    }

    return 0;
}

// ===== build.cpp compilation + execution =====

// Ensure the libc++ std module PCM is built and cached.
// Returns the path to the cached std.pcm, or an empty Path on failure.
// For Clang only — GCC handles import std via its gcm cache.
export Path ensure_std_pcm(const Toolchain& tc) {
    if (!tc.is_clang()) return Path();

    // Cache dir: ~/.cache/bake/ (parent of src/, which holds package sources)
    Path pcm_cache;
#ifdef _WIN32
    const char* home = std::getenv("LOCALAPPDATA");
    if (!home) home = "C:\\";
    pcm_cache = Path(home) / "bake";
#else
    const char* home = std::getenv("HOME");
    if (!home) home = "/tmp";
    pcm_cache = Path(home) / ".cache" / "bake";
#endif
    pcm_cache.mkdir_recursive();

    // Cache key: hash of compiler path + version output
    auto ver = run_process({tc.cxx_path, "--version"}, Path(), true);
    std::string key_data = tc.cxx_path + "\n" + ver.stdout_output;
    std::string key = SHA256::hex(key_data).substr(0, 16);

    Path std_pcm = pcm_cache / ("std-" + key + ".pcm");
    if (std_pcm.is_regular_file()) return std_pcm;  // cached

    // Locate std.cppm relative to the compiler: <prefix>/share/libc++/v1/std.cppm
    Path cxx_dir = Path(tc.cxx_path).parent();       // bin/
    Path prefix = cxx_dir.parent();                   // llvm/<ver>/
    Path std_cppm = prefix / "share" / "libc++" / "v1" / "std.cppm";
    if (!std_cppm.is_regular_file()) return Path();   // not a libc++-shipping Clang

    // libc++ include directory: <prefix>/include/c++/v1
    Path libcxx_inc = prefix / "include" / "c++" / "v1";

    std::println("bake: building std module (one-time)...");

    std::vector<std::string> cmd;
    cmd.push_back(tc.cxx_path);
    cmd.push_back("-std=c++23");
    cmd.push_back("-stdlib=libc++");
    cmd.push_back("-nostdinc++");
    cmd.push_back("-I" + libcxx_inc.string());
    cmd.push_back("-Wno-reserved-module-identifier");
    cmd.push_back("-c");
    cmd.push_back(std_cppm.string());
    cmd.push_back("--precompile");
    cmd.push_back("-o");
    cmd.push_back(std_pcm.string());

    auto result = run_process(cmd, Path(), true);
    if (!result.success()) {
        std::print(std::cerr, "{}", result.stderr_output);
        std::println(std::cerr, "bake: failed to pre-build std module");
        return Path();
    }

    return std_pcm;
}


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
        std::println(std::cerr, "bake: cannot find bake.build.cppm or bake_cabi.h");
        return 1;
    }
    write_file(wrapper_dst, *wrapper_content);
    write_file(cabi_dst, *cabi_content);

    // Pre-build the libc++ std module PCM (cached, one-time per compiler).
    auto std_pcm = ensure_std_pcm(tc);

    // Step 1: Compile wrapper module → BMI + .o
    Path pcm = bake_dir / "bake.build.pcm";
    Path wrapper_o = bake_dir / "bake.build.o";

    std::println("bake: compiling build script...");

    {
        std::vector<std::string> cmd;
        cmd.push_back(tc.cxx_path);
        cmd.push_back("-c");
        cmd.push_back("-std=c++23");
        if (tc.is_clang()) {
            cmd.push_back("-stdlib=libc++");
            cmd.push_back("-Wno-reserved-module-identifier");
        }
        cmd.push_back("-x");
        cmd.push_back("c++-module");
        cmd.push_back("-I" + bake_dir.string());
        if (tc.is_clang() && std_pcm.is_regular_file()) {
            cmd.push_back("-fmodule-file=std=" + std_pcm.string());
        }
        cmd.push_back("-fmodule-output=" + pcm.string());
        cmd.push_back(wrapper_dst.string());
        cmd.push_back("-o");
        cmd.push_back(wrapper_o.string());

        auto result = run_process(cmd, root, true);
        if (!result.success()) {
            std::print(std::cerr, "{}", result.stderr_output);
            std::println(std::cerr, "bake: failed to compile bake.build.cppm");
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
        if (tc.is_clang()) {
            cmd.push_back("-stdlib=libc++");
            cmd.push_back("-Wno-reserved-module-identifier");
        }
        cmd.push_back("-I" + bake_dir.string());
        if (tc.is_clang()) {
            if (std_pcm.is_regular_file()) {
                cmd.push_back("-fmodule-file=std=" + std_pcm.string());
            }
            cmd.push_back("-fmodule-file=bake.build=" + pcm.string());
        }
        cmd.push_back(build_cpp.string());
        cmd.push_back("-o");
        cmd.push_back(build_o.string());

        auto result = run_process(cmd, root, true);
        if (!result.success()) {
            std::print(std::cerr, "{}", result.stderr_output);
            std::println(std::cerr, "bake: failed to compile build.cpp");
            return 1;
        }
    }

    // Step 3: Link → build_app
    Path build_app = bake_dir / "build_app";
#ifdef _WIN32
    build_app = bake_dir / "build_app.exe";
#endif
    {
        std::vector<std::string> cmd;
        cmd.push_back(tc.cxx_path);
        if (tc.is_clang()) {
            cmd.push_back("-stdlib=libc++");
        }
        cmd.push_back(wrapper_o.string());
        cmd.push_back(build_o.string());
#ifdef _WIN32
        // Windows: link against the import library directly
        cmd.push_back(std::string(BAKE_LIB_DIR) + "/bake.lib");
#else
        cmd.push_back("-L" + std::string(BAKE_LIB_DIR));
        cmd.push_back("-lbake");
        cmd.push_back("-Wl,-rpath," + std::string(BAKE_LIB_DIR));
#endif
        cmd.push_back("-o");
        cmd.push_back(build_app.string());

        auto result = run_process(cmd, root, true);
        if (!result.success()) {
            std::print(std::cerr, "{}", result.stderr_output);
            std::println(std::cerr, "bake: failed to link build_app");
            return 1;
        }
    }

    // Step 4: Run build_app → .bake/build.json
    {
        auto result = run_process({build_app.string()}, root, true);
        if (!result.success()) {
            std::print(std::cerr, "{}", result.stderr_output);
            std::println(std::cerr, "bake: build_app failed");
            return 1;
        }
    }

    // Step 5: Read build.json → execute
    Path build_json = bake_dir / "build.json";
    if (!build_json.is_regular_file()) {
        std::println(std::cerr, "bake: build_app did not produce .bake/build.json");
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

// ===== Lock enforcement =====

// Check and resolve the lockfile. Returns 0 if the lock is consistent
// or was successfully (re)resolved. Returns non-zero on failure.
// On success, root/bake.lock is guaranteed to exist and be consistent.
// Cache integrity is verified on ALL builds to detect tampering.
export int enforce_lock(const Path& root, const Manifest& manifest, const ParsedArgs& args) {
    if (Lockfile::has_only_path_deps(manifest)) return 0;

    bool offline = args.has_option("offline") || args.has_option("frozen");
    bool locked = args.has_option("locked") || args.has_option("frozen");

    Path lock_path = root / "bake.lock";
    auto lockfile = Lockfile::load(lock_path);

    bool needs_resolve = false;
    if (!lockfile) {
        needs_resolve = true;
    } else if (!lockfile->is_consistent(manifest)) {
        needs_resolve = true;
    }

    // Verify cache integrity on all builds, not just frozen/locked.
    // This detects tampered or corrupted cached dependencies before they
    // enter the build graph.
    if (!needs_resolve && lockfile) {
        if (!verify_lock_cache(*lockfile, get_cache_dir())) {
            std::println(std::cerr,
                "bake: cache verification failed — cached sources missing or corrupted");
            // We must NOT re-resolve tags — that would move locked commits.
            // Instead, re-download using the existing locked commit hashes.
            // For now, report the error clearly.
            if (locked || offline) return 1;
            // Re-download using locked commits (not re-resolve)
            Resolver resolver;
            auto re_downloaded = resolver.redownload(*lockfile, ResolverConfig{});
            if (!re_downloaded || !verify_lock_cache(*lockfile, get_cache_dir())) {
                std::println(std::cerr,
                    "bake: re-download failed. Run 'bake update' to re-resolve from scratch.");
                return 1;
            }
            std::println("bake: cached sources re-downloaded using locked commits");
        }
    }

    if (!needs_resolve) return 0;

    if (locked) {
        std::println(std::cerr, "bake: lock file missing or stale (--locked)");
        return 1;
    }
    if (offline) {
        std::println(std::cerr, "bake: cannot resolve dependencies in offline mode");
        return 1;
    }

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
    std::println("bake: dependencies resolved and locked");
    return 0;
}

// ===== Dep source extraction (bridges bake.package types to bake.engine types) =====

// Extract bake-native dep sources and include dirs from a lockfile.
// Walks ALL lock nodes (root + transitive) in topological order so that
// dependencies are compiled before dependents.
// CMake deps (native=false) are skipped — Phase 4 territory.
static void extract_dep_info(
        const Lockfile& lockfile,
        const Path& cache_dir,
        std::vector<DepSourceEntry>& dep_sources,
        std::vector<Path>& dep_include_dirs) {

    // Topological sort of lock nodes by their dependency edges.
    // Nodes with no dependencies come first.
    std::vector<std::string> topo_order;
    {
        std::map<std::string, int> in_degree;
        std::map<std::string, std::vector<std::string>> dependents;

        for (auto& [id, node] : lockfile.nodes) {
            in_degree[id] = 0;
        }
        for (auto& [id, node] : lockfile.nodes) {
            for (auto& child_id : node.dependencies) {
                if (lockfile.nodes.count(child_id)) {
                    dependents[child_id].push_back(id);
                    in_degree[id]++;
                }
            }
        }

        // Kahn's algorithm
        std::vector<std::string> queue;
        for (auto& [id, deg] : in_degree) {
            if (deg == 0) queue.push_back(id);
        }
        std::sort(queue.begin(), queue.end());

        while (!queue.empty()) {
            std::string node_id = queue.front();
            queue.erase(queue.begin());
            topo_order.push_back(node_id);

            for (auto& dep_id : dependents[node_id]) {
                if (--in_degree[dep_id] == 0) {
                    queue.push_back(dep_id);
                }
            }
            std::sort(queue.begin(), queue.end());
        }

        // Append any remaining (cycle) nodes
        for (auto& [id, node] : lockfile.nodes) {
            if (std::find(topo_order.begin(), topo_order.end(), id) == topo_order.end()) {
                topo_order.push_back(id);
            }
        }
    }

    // Walk nodes in topological order
    for (auto& node_id : topo_order) {
        auto node_it = lockfile.nodes.find(node_id);
        if (node_it == lockfile.nodes.end()) continue;
        const auto& node = node_it->second;
        if (!node.native) continue;
        if (node.tree_sha256.empty()) continue;

        Path dep_cache = cache_dir / node.tree_sha256;
        if (!dep_cache.is_directory()) {
            std::println(std::cerr, "bake: cached source for '{}' not found at {}",
                         node_id, dep_cache.string());
            continue;
        }

        // Add dep's public/ to include dirs
        Path dep_public = dep_cache / "public";
        if (dep_public.is_directory()) {
            dep_include_dirs.push_back(dep_public);
        }

        // Discover dep's source files (.cpp, .c only — no modules in Phase 3)
        Path dep_src = dep_cache / "src";
        if (dep_src.is_directory()) {
            auto dep_disc = discover_sources(dep_src, dep_public);
            for (auto& cpp : dep_disc.cpp_files) {
                dep_sources.push_back({node_id, cpp});
            }
            for (auto& c : dep_disc.c_files) {
                dep_sources.push_back({node_id, c});
            }
        }
    }
}

// ===== build command =====

export int cmd_build(const ParsedArgs& args) {
    // Find project root
    auto root = find_project_root();
    if (!root) {
        std::println(std::cerr, "bake: no bake.toml found in current directory or any parent");
        return 1;
    }

    // Load manifest (needed for lock enforcement before build.cpp dispatch)
    auto manifest = Manifest::load(*root);
    if (!manifest) {
        std::println(std::cerr, "bake: failed to load bake.toml");
        return 1;
    }

    // ===== Lock enforcement (before build.cpp so --locked/--frozen work universally)
    if (int rc = enforce_lock(*root, *manifest, args); rc != 0) return rc;

    // Check for build.cpp (escape hatch mode)
    Path build_cpp = *root / "build.cpp";
    if (build_cpp.is_regular_file()) {
        return build_with_build_cpp(*root, args);
    }

    // Handle workspace: build all members with inter-member deps
    if (manifest->is_workspace()) {
        int result = 0;
        int jobs = 0;
        if (auto j = args.get_option("j")) {
            jobs = std::atoi(j->c_str());
        }

        // Enforce lock for the workspace: merge all members' remote deps into
        // a single combined manifest, then resolve once. This prevents each
        // member's enforce_lock from overwriting the shared bake.lock.
        {
            Manifest combined;
            combined.project_dir = *root;
            for (auto& member : manifest->workspace->members) {
                Path member_dir = *root / member;
                auto member_manifest = Manifest::load(member_dir);
                if (!member_manifest) continue;
                for (auto& [name, dep] : member_manifest->dependencies) {
                    if (dep.is_path_dep) continue;
                    // Detect conflicts: same dep name but different URL/tag
                    auto existing = combined.dependencies.find(name);
                    if (existing != combined.dependencies.end()) {
                        if (existing->second.url != dep.url || existing->second.tag != dep.tag) {
                            std::println(std::cerr,
                                "bake: dependency conflict — '{}' declared differently:\n"
                                "  member '{}': url=\"{}\", tag=\"{}\"\n"
                                "  member '{}': url=\"{}\", tag=\"{}\"",
                                name,
                                /* find which member declared the existing one */
                                "(earlier)", existing->second.url, existing->second.tag,
                                member, dep.url, dep.tag);
                            return 1;
                        }
                        // Same URL + tag — reuse, skip
                        continue;
                    }
                    combined.dependencies[name] = dep;
                }
            }
            if (!Lockfile::has_only_path_deps(combined)) {
                if (int rc = enforce_lock(*root, combined, args); rc != 0) {
                    return rc;
                }
            }
        }

        // Load the final lockfile (may have been updated by member enforcement)
        auto lockfile = Lockfile::load(*root / "bake.lock");

        // Extract dep sources + include dirs from lockfile
        std::vector<DepSourceEntry> dep_sources;
        std::vector<Path> dep_include_dirs;
        if (lockfile && !lockfile->empty()) {
            extract_dep_info(*lockfile, get_cache_dir(), dep_sources, dep_include_dirs);
        }

        // Track built members for inter-member dependency resolution
        struct BuiltMember {
            std::string name;
            Path bmi_dir;
            Path lib_path;
            std::vector<std::string> module_names;
        };
        std::vector<BuiltMember> built;

        // Pre-build std module PCM once for all members (cached, one-time)
        auto std_pcm = ensure_std_pcm(Toolchain::detect());

        for (auto& member : manifest->workspace->members) {
            Path member_dir = *root / member;
            auto member_manifest = Manifest::load(member_dir);
            if (!member_manifest || !member_manifest->has_package()) {
                std::println(std::cerr, "bake: skipping workspace member '{}' (no [package])", member);
                continue;
            }

            // Check -p flag
            if (auto p = args.get_option("p")) {
                if (*p != member_manifest->package->name) continue;
            }

            std::println("bake: building '{}'", member_manifest->package->name);
            auto layout = Layout::detect(member_dir, *root);
            auto tc = Toolchain::detect();

            auto plan = create_convention_plan(*member_manifest, layout, tc,
                                                member_manifest->options,
                                                dep_sources, dep_include_dirs,
                                                /*compile_path_deps=*/false,
                                                std_pcm);

            // Inject external modules from built dependencies
            for (auto& action : plan.actions) {
                if (action.type != BuildAction::Type::Compile &&
                    action.type != BuildAction::Type::CompileModule) continue;

                for (auto& bm : built) {
                    if (!bm.bmi_dir.is_directory()) continue;
                    for (auto& mod_name : bm.module_names) {
                        Path bmi = bm.bmi_dir / (mod_name + ".pcm");
                        if (bmi.is_regular_file()) {
                            action.command.push_back("-fmodule-file=" + mod_name + "=" + bmi.string());
                        }
                    }
                }
                // Also inject std PCM so transitive import std; resolves
                if (std_pcm.is_regular_file()) {
                    action.command.push_back("-fmodule-file=std=" + std_pcm.string());
                    // Clang needs libc++ for import std
                    if (tc.is_clang()) {
                        action.command.push_back("-stdlib=libc++");
                        action.command.push_back("-Wno-reserved-module-identifier");
                    }
                }
            }

            // Inject library links from built dependencies
            for (auto& action : plan.actions) {
                if (action.type != BuildAction::Type::Link) continue;

                // Clang needs -stdlib=libc++ when import std is in use
                if (std_pcm.is_regular_file() && tc.is_clang()) {
                    action.command.insert(action.command.begin() + 1, "-stdlib=libc++");
                }

                for (auto& bm : built) {
                    if (bm.lib_path.string().empty()) continue;
                    Path lib_dir = bm.lib_path.parent();
#ifdef _WIN32
                    // Windows: link the .lib directly
                    action.command.insert(action.command.end() - 2,
                        (lib_dir / (bm.name + ".lib")).string());
#else
                    action.command.insert(action.command.end() - 2,
                        {"-L" + lib_dir.string(), "-l" + bm.name});
                    action.command.insert(action.command.end() - 2,
                        "-Wl,-rpath," + lib_dir.string());
#endif
                }
            }

            int r = execute_plan(plan, jobs);
            if (r != 0) { result = r; break; }

            // Record this member for dependents
            BuiltMember bm;
            bm.name = member_manifest->package->name;
            bm.bmi_dir = layout.bmi_dir;
            bm.lib_path = plan.primary_output;

            // Collect module names from BMI directory
            if (bm.bmi_dir.is_directory()) {
                for (auto& entry : std::filesystem::directory_iterator(bm.bmi_dir.fs())) {
                    if (entry.path().extension() == ".pcm") {
                        bm.module_names.push_back(entry.path().stem().string());
                    }
                }
            }
            built.push_back(std::move(bm));
        }
        return result;
    }

    if (!manifest->has_package()) {
        std::println(std::cerr, "bake: no [package] section in bake.toml");
        return 1;
    }

    // Single package build
    auto layout = Layout::detect(*root);
    auto tc = Toolchain::detect();

    std::println("bake: detected {} ({})", tc.kind_name(), tc.cxx_path);

    // Pre-build std module PCM for import std support (cached, one-time)
    auto std_pcm = ensure_std_pcm(tc);

    // Extract dep sources + include dirs from lockfile
    std::vector<DepSourceEntry> dep_sources;
    std::vector<Path> dep_include_dirs;
    auto lockfile = Lockfile::load(*root / "bake.lock");
    if (lockfile && !lockfile->empty()) {
        extract_dep_info(*lockfile, get_cache_dir(), dep_sources, dep_include_dirs);
    }

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

    auto plan = create_convention_plan(*manifest, layout, tc, options,
                                        dep_sources, dep_include_dirs,
                                        /*compile_path_deps=*/true,
                                        std_pcm);

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

    // Remove .bake/ (build script staging) for root and workspace members
    auto manifest = Manifest::load(*root);
    if (manifest && manifest->is_workspace()) {
        for (auto& member : manifest->workspace->members) {
            Path member_dir = *root / member;
            Path member_bake = member_dir / ".bake";
            if (member_bake.exists()) {
                member_bake.remove_all();
                std::println("Removed {}", member_bake.string());
                removed++;
            }
        }
    } else {
        Path bake_dir = *root / ".bake";
        if (bake_dir.exists()) {
            bake_dir.remove_all();
            std::println("Removed {}", bake_dir.string());
            removed++;
        }
    }

    if (removed == 0) {
        std::println("Nothing to clean");
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
            std::println(std::cerr, "bake: cannot run non-executable package");
            return 1;
        }

        auto layout = Layout::detect(*root);
        std::string exe_name = library_name(manifest->package->name, PackageType::Executable);
        exe_path = layout.bin_dir / exe_name;
    }

    if (!exe_path.exists()) {
        std::println(std::cerr, "bake: built executable not found at {}", exe_path.string());
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
            if (auto* deps = tbl["dependencies"].as_table()) {
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
        std::println(std::cerr, "bake: failed to write bake.toml");
        return 1;
    }

    std::println("Added dependency '{}' = {{ url = \"{}\", tag = \"{}\" }}",
                name, url, tag);
    std::println("Run 'bake build' to resolve and download.");
    return 0;
}

// ===== update command =====

export int cmd_update(const ParsedArgs& args) {
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
        if (manifest->dependencies.find(filter_dep) == manifest->dependencies.end()) {
            std::println(std::cerr, "bake: dependency '{}' not found in bake.toml", filter_dep);
            return 1;
        }
        auto& dep = manifest->dependencies[filter_dep];
        if (dep.is_path_dep) {
            std::println(std::cerr, "bake: '{}' is a path dependency (no resolution needed)", filter_dep);
            return 1;
        }
    }

    Path lock_path = *root / "bake.lock";

    // Load existing lock for comparison / preservation
    auto old_lock = Lockfile::load(lock_path);

    Resolver resolver;

    if (filter_dep.empty()) {
        // Full re-resolve (existing behavior)
        auto new_lock = resolver.resolve(*manifest, ResolverConfig{});
        if (!new_lock) {
            std::println(std::cerr, "bake: failed to resolve dependencies");
            return 1;
        }

        // Report changes
        if (old_lock) {
            for (auto& [name, node_id] : new_lock->root_deps) {
                auto old_it = old_lock->root_deps.find(name);
                if (old_it != old_lock->root_deps.end()) {
                    auto& old_node = old_lock->nodes[old_it->second];
                    auto& new_node = new_lock->nodes[node_id];
                    if (old_node.commit != new_node.commit) {
                        std::println("bake: {} updated: {} → {}",
                                    name, old_node.commit.substr(0, 12),
                                    new_node.commit.substr(0, 12));
                    }
                } else {
                    std::println("bake: {} added", name);
                }
            }
        }

        if (!new_lock->save(lock_path)) {
            std::println(std::cerr, "bake: failed to write bake.lock");
            return 1;
        }
    } else {
        // Single-dep update: resolve the dep AND its transitive closure,
        // then prune old transitive nodes and merge new ones.
        //
        // If the old lock is missing or corrupt, we can't safely do a
        // partial update (we'd lose other root deps). Fall back to full resolve.
        if (!old_lock || !old_lock->is_consistent(*manifest)) {
            std::println("bake: lock missing or stale, doing full re-resolve");
            auto new_lock = resolver.resolve(*manifest, ResolverConfig{});
            if (!new_lock) {
                std::println(std::cerr, "bake: failed to resolve dependencies");
                return 1;
            }
            if (!new_lock->save(lock_path)) {
                std::println(std::cerr, "bake: failed to write bake.lock");
                return 1;
            }
            std::println("bake: lock file updated");
            return 0;
        }

        auto& dep = manifest->dependencies[filter_dep];

        // Resolve dep + full transitive subtree
        auto subtree = resolver.resolve_subtree(dep, filter_dep);
        if (!subtree) {
            std::println(std::cerr, "bake: failed to resolve '{}'", filter_dep);
            return 1;
        }

        // Report change
        if (old_lock) {
            auto old_it = old_lock->root_deps.find(filter_dep);
            if (old_it != old_lock->root_deps.end()) {
                auto& old_node = old_lock->nodes[old_it->second];
                auto& new_node = subtree->nodes[subtree->root_deps[filter_dep]];
                if (old_node.commit != new_node.commit) {
                    std::println("bake: {} updated: {} → {}",
                                filter_dep, old_node.commit.substr(0, 12),
                                new_node.commit.substr(0, 12));
                } else {
                    std::println("bake: {} unchanged ({})", filter_dep,
                                new_node.commit.substr(0, 12));
                }
            } else {
                std::println("bake: {} added", filter_dep);
            }
        }

        // Build the merged lock:
        // 1. Start with a fresh lockfile
        // 2. Copy all old root_deps except the updated one
        // 3. Copy all old nodes except those exclusively reachable from the old dep
        // 4. Add all new nodes from the subtree
        Lockfile new_lock;

        // Find the old node ID for the updated dep
        std::string old_node_id;
        if (old_lock) {
            auto old_it = old_lock->root_deps.find(filter_dep);
            if (old_it != old_lock->root_deps.end()) {
                old_node_id = old_it->second;
            }
        }

        // Compute set of nodes reachable from old_node_id (the old subtree)
        std::set<std::string> old_subtree_nodes;
        if (!old_node_id.empty() && old_lock) {
            std::vector<std::string> queue = {old_node_id};
            while (!queue.empty()) {
                std::string nid = queue.back();
                queue.pop_back();
                if (old_subtree_nodes.count(nid)) continue;
                old_subtree_nodes.insert(nid);
                auto n_it = old_lock->nodes.find(nid);
                if (n_it != old_lock->nodes.end()) {
                    for (auto& child : n_it->second.dependencies) {
                        queue.push_back(child);
                    }
                }
            }
        }

        // Compute set of nodes reachable from OTHER root deps (keep these)
        std::set<std::string> keep_nodes;
        if (old_lock) {
            for (auto& [name, nid] : old_lock->root_deps) {
                if (name == filter_dep) continue;
                std::vector<std::string> queue = {nid};
                while (!queue.empty()) {
                    std::string id = queue.back();
                    queue.pop_back();
                    if (keep_nodes.count(id)) continue;
                    keep_nodes.insert(id);
                    auto n_it = old_lock->nodes.find(id);
                    if (n_it != old_lock->nodes.end()) {
                        for (auto& child : n_it->second.dependencies) {
                            queue.push_back(child);
                        }
                    }
                }
            }
        }

        // Copy old root_deps (except the updated dep)
        if (old_lock) {
            for (auto& [name, nid] : old_lock->root_deps) {
                if (name == filter_dep) continue;
                new_lock.root_deps[name] = nid;
            }
        }

        // Copy old nodes that are NOT exclusively in the old subtree
        if (old_lock) {
            for (auto& [id, node] : old_lock->nodes) {
                // Keep if not in old subtree, or if also reachable from other deps
                if (old_subtree_nodes.count(id) && !keep_nodes.count(id)) {
                    continue; // Prune stale transitive node
                }
                new_lock.nodes[id] = node;
            }
        }

        // Add all new nodes from the subtree
        for (auto& [id, node] : subtree->nodes) {
            new_lock.nodes[id] = node;
        }
        new_lock.root_deps[filter_dep] = subtree->root_deps[filter_dep];

        if (!new_lock.save(lock_path)) {
            std::println(std::cerr, "bake: failed to write bake.lock");
            return 1;
        }
    }

    std::println("bake: lock file updated");
    return 0;
}

// ===== Stub commands =====

export int cmd_test(const ParsedArgs& args) {
    std::println(std::cerr, "bake: test not yet implemented (Phase 7)");
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

    std::println(std::cerr, "bake: unknown command '{}'", args.command);
    std::println(std::cerr, "Run 'bake --help' for usage.");
    return 1;
}

} // namespace bake::cli
