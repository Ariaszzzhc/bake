export module bake.foreign;

import std;
import bake.util;
import bake.compiler;
import bake.project;

// ============================================================
// bake.foreign — CMake bridge: configure + build + install + extract usage
// ============================================================

namespace bake {

// ===== Usage requirements collected from a foreign build =====

export struct UsageRequirements {
    std::vector<std::string> includes;       // -I paths
    std::vector<std::string> defines;        // -D KEY=VALUE entries
    std::vector<Path> libraries;             // full paths to .a / .so / .dylib
    std::vector<std::string> link_flags;     // additional linker flags
};

// ===== CMake configuration =====

export struct CMakeConfig {
    Path source_dir;         // where CMakeLists.txt lives
    Path staging_dir;        // CMAKE_INSTALL_PREFIX target
    std::string build_config = "Release";    // Debug | Release | RelWithDebInfo
    std::vector<std::pair<std::string, std::string>> defines;  // -D key=value
    const Toolchain* toolchain = nullptr;
    std::vector<std::string> locked_packages;  // names allowed for find_package
};

// ===== Sandbox flags =====

// Generate CMake -D flags for best-effort sandboxing.
// These restrict standard find_package / FetchContent paths.
export inline std::vector<std::string> cmake_sandbox_flags(const CMakeConfig& config) {
    std::vector<std::string> flags;
    flags.push_back("-DFETCHCONTENT_FULLY_DISCONNECTED=ON");

    // Disable find_package for common packages NOT in the lock.
    // This is a best-effort list — CMake scripts can still bypass via
    // file(DOWNLOAD), ExternalProject_Add, execute_process, etc.
    static const std::vector<std::string> common_packages = {
        "ZLIB", "PNG", "JPEG", "TIFF", "BZip2", "LZMA", "Zstd",
        "OpenSSL", "CURL", "EXPAT", "Freetype", "SQLite3",
        "Boost", "Python", "Java", "Threads", "MPI"
    };

    for (const auto& pkg : common_packages) {
        // If this package is in the locked list, don't disable it
        bool locked = false;
        for (const auto& lp : config.locked_packages) {
            if (to_lower(lp) == to_lower(pkg)) {
                locked = true;
                break;
            }
        }
        if (!locked) {
            flags.push_back("-DCMAKE_DISABLE_FIND_PACKAGE_" + pkg + "=ON");
        }
    }

    return flags;
}

// ===== Internal helpers =====

namespace detail {

// Locate the cmake executable on PATH.
inline std::optional<std::string> find_cmake() {
    if (auto p = find_in_path("cmake")) {
        return p->string();
    }
    return std::nullopt;
}

// Run cmake configure: cmake -S <src> -B <build> -DCMAKE_INSTALL_PREFIX=<staging> <flags>
inline bool cmake_configure(const std::string& cmake_exe,
                            const CMakeConfig& config,
                            const Path& build_dir) {
    std::vector<std::string> cmd;
    cmd.push_back(cmake_exe);
    cmd.push_back("-S");
    cmd.push_back(config.source_dir.string());
    cmd.push_back("-B");
    cmd.push_back(build_dir.string());
    cmd.push_back("-DCMAKE_INSTALL_PREFIX=" + config.staging_dir.string());
    cmd.push_back("-DCMAKE_BUILD_TYPE=" + config.build_config);

    // Default to static libraries unless overridden by user defines
    bool has_build_shared = false;
    for (auto& [k, v] : config.defines) {
        if (k == "BUILD_SHARED_LIBS") {
            has_build_shared = true;
            break;
        }
    }
    if (!has_build_shared) {
        cmd.push_back("-DBUILD_SHARED_LIBS=OFF");
    }

    // Sandbox flags
    for (auto& f : cmake_sandbox_flags(config)) {
        cmd.push_back(f);
    }

    // User defines (applied last so they override defaults)
    for (auto& [key, value] : config.defines) {
        cmd.push_back("-D" + key + "=" + value);
    }

    // Inject CC/CXX so CMake uses the same compiler bake detected.
    // This is done via environment variables in run_process, but since
    // our run_process doesn't support custom env vars, we pass them as
    // CMAKE_C_COMPILER / CMAKE_CXX_COMPILER if not already set by user.
    if (config.toolchain) {
        bool has_c_compiler = false;
        bool has_cxx_compiler = false;
        for (auto& [k, v] : config.defines) {
            if (k == "CMAKE_C_COMPILER") has_c_compiler = true;
            if (k == "CMAKE_CXX_COMPILER") has_cxx_compiler = true;
        }
        if (!has_c_compiler && !config.toolchain->cc_path.empty()) {
            cmd.push_back("-DCMAKE_C_COMPILER=" + config.toolchain->cc_path);
        }
        if (!has_cxx_compiler && !config.toolchain->cxx_path.empty()) {
            cmd.push_back("-DCMAKE_CXX_COMPILER=" + config.toolchain->cxx_path);
        }
    }

    auto result = run_process(cmd, Path(), true);
    if (!result.success()) {
        std::print(std::cerr, "{}", result.stderr_output);
        std::println(std::cerr, "bake: CMake configure failed for {}",
                     config.source_dir.string());
        return false;
    }
    return true;
}

// Run cmake build + install: cmake --build <build> --target install
inline bool cmake_build_install(const std::string& cmake_exe,
                                const Path& build_dir) {
    std::vector<std::string> cmd;
    cmd.push_back(cmake_exe);
    cmd.push_back("--build");
    cmd.push_back(build_dir.string());
    cmd.push_back("--target");
    cmd.push_back("install");
    cmd.push_back("--config");
    cmd.push_back("Release");

    auto result = run_process(cmd, Path(), true);
    if (!result.success()) {
        std::print(std::cerr, "{}", result.stderr_output);
        std::println(std::cerr, "bake: CMake build/install failed");
        return false;
    }
    return true;
}

// Scan the install tree for include directories and library files.
inline UsageRequirements scan_install_tree(const Path& staging_dir) {
    UsageRequirements usage;

    // Include directories
    Path inc_dir = staging_dir / "include";
    if (inc_dir.is_directory()) {
        usage.includes.push_back(inc_dir.string());
    }

    // Library files: scan lib/ and lib/<variant>/ directories
    Path lib_dir = staging_dir / "lib";
    if (lib_dir.is_directory()) {
        for (auto& entry : std::filesystem::recursive_directory_iterator(lib_dir.fs())) {
            if (!entry.is_regular_file()) continue;
            std::string ext = entry.path().extension().string();
            if (ext == ".a" || ext == ".so" || ext == ".dylib" ||
                ext == ".lib" || ext == ".dll") {
                usage.libraries.push_back(Path(entry.path()));
            }
        }
    }

    // Also check for cmake config directories — they may contain
    // additional usage requirements we could parse in the future.
    // For Phase 4 MVP, we rely on include + lib scanning.

    return usage;
}

} // namespace detail

// ===== Public API =====

// Run the full CMake bridge: configure → build → install → scan install tree.
// Returns UsageRequirements on success, nullopt on failure.
export inline std::optional<UsageRequirements> run_cmake_bridge(const CMakeConfig& config) {
    // Verify cmake is available
    auto cmake_exe = detail::find_cmake();
    if (!cmake_exe) {
        std::println(std::cerr, "bake: cmake not found on PATH");
        return std::nullopt;
    }

    // Verify CMakeLists.txt exists
    Path cmake_lists = config.source_dir / "CMakeLists.txt";
    if (!cmake_lists.is_regular_file()) {
        std::println(std::cerr, "bake: no CMakeLists.txt in {}",
                     config.source_dir.string());
        return std::nullopt;
    }

    // Prepare directories
    Path build_dir = config.staging_dir / "build";
    Path install_dir = config.staging_dir;  // CMAKE_INSTALL_PREFIX = staging itself

    // Clean previous build if exists (simple approach — no incremental yet)
    if (build_dir.is_directory()) {
        build_dir.remove_all();
    }
    build_dir.mkdir_recursive();

    // Step 1: Configure
    if (!detail::cmake_configure(*cmake_exe, config, build_dir)) {
        return std::nullopt;
    }

    // Step 2: Build + Install
    if (!detail::cmake_build_install(*cmake_exe, build_dir)) {
        return std::nullopt;
    }

    // Step 3: Scan install tree for usage requirements
    auto usage = detail::scan_install_tree(install_dir);

    if (usage.includes.empty() && usage.libraries.empty()) {
        std::println(std::cerr,
            "bake: CMake build succeeded but no includes or libraries found in {}",
            install_dir.string());
        // Still return the (empty) usage — the dep may be header-only or
        // install to non-standard locations.
    }

    return usage;
}

} // namespace bake
