export module bake.compiler;

import std;
import bake.util;
import bake.project;

// Phase 5: LLVM compiler integration — C ABI
extern "C" {
    int bake_has_llvm(void);
}

// ============================================================
// bake.compiler — toolchain detection, compile/link commands
// ============================================================

namespace bake {

export enum class CompilerKind {
    Clang,
    Gcc,
    AppleClang,
    BakeSelf,   // bake itself acting as compiler (bake c++ / bake cc)
    Unknown,
};

export struct Toolchain {
    CompilerKind kind = CompilerKind::Unknown;
    std::string cxx_path;       // C++ compiler
    std::string cc_path;        // C compiler
    std::string ar_path;        // archiver
    std::string scanner_path;   // clang-scan-deps (Clang only)
    // Vendored system header dirs for the module scanner. clang-scan-deps
    // runs the scan in-process and never enters bake's driver, so the
    // driver's default -isystem injection must be repeated here (BakeSelf).
    std::vector<std::string> scan_include_dirs;

    bool has_scanner() const { return !scanner_path.empty(); }
    bool is_clang() const { return kind == CompilerKind::Clang || kind == CompilerKind::AppleClang || kind == CompilerKind::BakeSelf; }
    bool is_gcc() const { return kind == CompilerKind::Gcc; }

    static Toolchain detect() {
        Toolchain tc;

        // 0. Self-spawn: if bake was built with LLVM, use bake c++ as the compiler
        if (bake_has_llvm()) {
            tc.kind = CompilerKind::BakeSelf;
            // When running inside build_app (linked to libbake), get_self_exe_path()
            // returns build_app's path, not bake's. Use BAKE_EXE env var if set.
            std::string self;
            if (const char* bake_exe = std::getenv("BAKE_EXE")) {
                self = bake_exe;
            } else {
                self = get_self_exe_path();
            }
            if (self.empty()) self = "bake";  // graceful fallback
            tc.cxx_path = self;
            tc.cc_path = self;
            // clang-scan-deps from LLVM prefix (runtime resolution).
            {
                Path llvm_prefix = find_llvm_prefix();
                if (!llvm_prefix.string().empty()) {
                    Path scanner = llvm_prefix / "bin" / "clang-scan-deps";
                    if (scanner.is_regular_file())
                        tc.scanner_path = scanner.string();
                }
            }
            // The scanner bypasses bake's driver, so repeat the vendored
            // header search paths here (same order as bake_clang_driver.cpp).
            {
                Path lib = find_lib_dir();
                if (!lib.string().empty()) {
                    Path libcxx = lib / "libcxx" / "include";
                    if (libcxx.is_directory())
                        tc.scan_include_dirs.push_back(libcxx.string());
                    Path darwin_inc = lib / "libc" / "darwin" / "include";
                    if (darwin_inc.is_directory())
                        tc.scan_include_dirs.push_back(darwin_inc.string());
                }
            }
            {
                Path rd = find_clang_resource_dir();
                if (!rd.string().empty())
                    tc.scan_include_dirs.push_back(rd.string() + "/include");
            }
            tc.ar_path = "ar";     // still use system ar for now (bake ar is Phase 6)
            return tc;
        }

        // 1. Check CXX environment variable
        if (const char* cxx_env = std::getenv("CXX")) {
            if (Path(cxx_env).is_regular_file() || run_process({cxx_env, "--version"}, Path(), true).success()) {
                tc.cxx_path = cxx_env;
            }
        }

        // 2. Prefer clang++ on PATH (better module support than Apple Clang's c++)
        if (tc.cxx_path.empty()) {
            if (auto p = find_in_path("clang++")) {
                tc.cxx_path = p->string();
            }
        }

        // 3. Fall back to c++
        if (tc.cxx_path.empty()) {
            if (auto p = find_in_path("c++")) {
                tc.cxx_path = p->string();
            }
        }
        if (tc.cxx_path.empty()) {
            tc.cxx_path = "c++";
        }

        // Find cc compiler
        if (const char* cc_env = std::getenv("CC")) {
            tc.cc_path = cc_env;
        } else {
            if (auto p = find_in_path("clang")) {
                tc.cc_path = p->string();
            } else if (auto p = find_in_path("cc")) {
                tc.cc_path = p->string();
            }
        }
        if (tc.cc_path.empty()) tc.cc_path = "cc";

        // Find ar
        if (auto p = find_in_path("ar")) {
            tc.ar_path = p->string();
        }
        if (tc.ar_path.empty()) {
            tc.ar_path = "ar";
        }

        // Determine compiler kind from version output
        auto ver = run_process({tc.cxx_path, "--version"}, Path(), true);
        std::string ver_str = ver.stdout_output + ver.stderr_output;

        if (contains(ver_str, "Apple clang") || contains(ver_str, "Apple LLVM")) {
            tc.kind = CompilerKind::AppleClang;
        } else if (contains(ver_str, "clang")) {
            tc.kind = CompilerKind::Clang;
        } else if (contains(ver_str, "g++") || contains(ver_str, "gcc") ||
                   contains(ver_str, "GCC") || contains(ver_str, "Free Software Foundation")) {
            tc.kind = CompilerKind::Gcc;
        }

        // Find clang-scan-deps next to the compiler or on PATH
        if (tc.is_clang()) {
            auto parent = Path(tc.cxx_path).parent();
            auto candidate = parent / "clang-scan-deps";
            if (candidate.is_regular_file()) {
                tc.scanner_path = candidate.string();
            } else {
                if (auto p = find_in_path("clang-scan-deps")) {
                    tc.scanner_path = p->string();
                }
            }
        }

        return tc;
    }

    std::string kind_name() const {
        switch (kind) {
            case CompilerKind::Clang:       return "clang";
            case CompilerKind::AppleClang:  return "apple-clang";
            case CompilerKind::Gcc:         return "gcc";
            case CompilerKind::BakeSelf:    return "bake";
            default:                        return "unknown";
        }
    }
};

// Returns the argv prefix for invoking the C++ compiler driver.
// BakeSelf needs ["<bake_path>", "c++"]; other kinds just need ["<cxx_path>"].
export inline std::vector<std::string> cxx_prefix(const Toolchain& tc) {
    if (tc.kind == CompilerKind::BakeSelf) return {tc.cxx_path, "c++"};
    return {tc.cxx_path};
}

// ===== Compile configuration =====

export struct CompileConfig {
    Path source;
    Path output;           // .o file path
    std::string std_ver = "c++20";
    std::vector<Path> include_dirs;
    std::vector<std::pair<std::string, std::string>> defines;
    bool is_module_interface = false;
    Path bmi_output;       // BMI path (for module interfaces)
    // module dependencies: module_name → bmi_path
    std::vector<std::pair<std::string, Path>> module_deps;
    bool use_pic = false;  // -fPIC
    std::vector<std::string> extra_flags;  // raw compiler flags (e.g. -fno-rtti)
};

// The manifest/build API uses the compiler's standard spelling directly.
// A standard beginning with "c++" is C++; the remaining C spellings (c17,
// c23, gnu17, ...) select C when the source itself is a .c file.
export bool is_c_standard(std::string_view std_ver) {
    return (std_ver.starts_with("c") || std_ver.starts_with("gnu")) &&
           !std_ver.starts_with("c++") && !std_ver.starts_with("gnu++");
}

// ===== Link configuration =====

export struct LinkConfig {
    std::vector<Path> inputs;   // .o files
    Path output;                // output path
    PackageType type = PackageType::Executable;
    std::vector<std::string> link_libs;  // -l flags
    std::vector<std::string> frameworks; // Apple -framework flags
    bool use_cxx_linker = true;
};

// ===== Command generation =====

export std::vector<std::string> make_compile_command(const Toolchain& tc,
                                                      const CompileConfig& cc) {
    std::vector<std::string> cmd;
    const bool compile_as_c = cc.source.is_c() && !cc.is_module_interface;
    // BakeSelf spawns "<bake_path> cc|c++" as two argv elements.
    if (tc.kind == CompilerKind::BakeSelf) {
        cmd.push_back(tc.cxx_path);
        cmd.push_back(compile_as_c ? "cc" : "c++");
    } else {
        cmd.push_back(compile_as_c ? tc.cc_path : tc.cxx_path);
    }

    // Mode
    cmd.push_back("-c");

    // Standard
    cmd.push_back("-std=" + cc.std_ver);

    // When using import std; or import std.compat;, Clang requires libc++.
    // Detect this by checking if "std" or "std.compat" is in module_deps.
    bool needs_libcxx = false;
    if (!compile_as_c) {
        for (const auto& [mod_name, _] : cc.module_deps) {
            if (mod_name == "std" || mod_name == "std.compat") {
                needs_libcxx = true;
                break;
            }
        }
    }
    if (needs_libcxx && tc.is_clang()) {
        cmd.push_back("-stdlib=libc++");
        cmd.push_back("-Wno-reserved-module-identifier");
    }

    // PIC
    if (cc.use_pic) {
        cmd.push_back("-fPIC");
    }

    // Extra raw compiler flags (e.g. -fno-rtti for LLVM-interfacing sources)
    for (auto& flag : cc.extra_flags) {
        cmd.push_back(flag);
    }

    // Module interface
    if (cc.is_module_interface) {
        if (tc.is_clang()) {
            cmd.push_back("-x");
            cmd.push_back("c++-module");
            if (cc.bmi_output.string() != "") {
                cmd.push_back("-fmodule-output=" + cc.bmi_output.string());
            } else {
                cmd.push_back("-fmodule-output");
            }
        } else if (tc.is_gcc()) {
            cmd.push_back("-fmodules-ts");
        }
    }

    // Include dirs
    for (auto& inc : cc.include_dirs) {
        cmd.push_back("-I" + inc.string());
    }

    // Defines
    for (auto& [name, value] : cc.defines) {
        if (value.empty()) {
            cmd.push_back("-D" + name);
        } else {
            cmd.push_back("-D" + name + "=" + value);
        }
    }

    // Module dependencies (BMI references for consumers AND interfaces)
    if (!compile_as_c && !cc.module_deps.empty()) {
        for (auto& [mod_name, bmi_path] : cc.module_deps) {
            if (tc.is_clang()) {
                cmd.push_back("-fmodule-file=" + mod_name + "=" + bmi_path.string());
            }
            // GCC module consumption is handled differently (gcm cache)
            // For now, skip GCC module deps - they need -fmodule-mapper
        }
    }

    // Source file
    cmd.push_back(cc.source.string());

    // Output
    cmd.push_back("-o");
    cmd.push_back(cc.output.string());

    return cmd;
}

export std::vector<std::string> make_link_command(const Toolchain& tc,
                                                   const LinkConfig& lc) {
    std::vector<std::string> cmd;
    // BakeSelf spawns "<bake_path> cc|c++" as two argv elements (handles linking via in-process LLD).
    if (tc.kind == CompilerKind::BakeSelf) {
        cmd.push_back(tc.cxx_path);
        cmd.push_back(lc.use_cxx_linker ? "c++" : "cc");
    } else {
        cmd.push_back(lc.use_cxx_linker ? tc.cxx_path : tc.cc_path);
    }

    if (lc.type == PackageType::SharedLib) {
        cmd.push_back("-shared");
        cmd.push_back("-fPIC");
    }

    // Input files
    for (auto& input : lc.inputs) {
        cmd.push_back(input.string());
    }

    // Link libraries — full paths (containing / or ending in .a) are
    // passed directly; bare names get -l prefix.
    for (auto& lib : lc.link_libs) {
        if (lib.find('/') != std::string::npos || lib.ends_with(".a"))
            cmd.push_back(lib);
        else
            cmd.push_back("-l" + lib);
    }

    for (auto& framework : lc.frameworks) {
        cmd.push_back("-framework");
        cmd.push_back(framework);
    }

    // Output
    cmd.push_back("-o");
    cmd.push_back(lc.output.string());

    return cmd;
}

export std::vector<std::string> make_archive_command(const Toolchain& tc,
                                                      const LinkConfig& lc) {
    std::vector<std::string> cmd;
    cmd.push_back(tc.ar_path);
    cmd.push_back("rcs");  // replace, create, write index

    cmd.push_back(lc.output.string());

    for (auto& input : lc.inputs) {
        cmd.push_back(input.string());
    }

    return cmd;
}

// ===== Standard module (std / std.compat) PCM management =====

// Maps module name → prebuilt PCM path (e.g. {"std": ..., "std.compat": ...}).
export using ModuleFileMap = std::map<std::string, Path>;

// Generate a standard-module interface source from the vendored libc++
// module template. Reads <lib>/libcxx/modules/<name>.cppm.in, concatenates
// the sorted <name>/*.inc files, replaces exactly one placeholder, and writes
// <cache_dir>/<name>.cppm. Returns an empty Path on failure.
static Path generate_standard_module_source(
        const Path& cache_dir,
        std::string_view module_name,
        std::string_view placeholder) {
    Path modules_dir = find_lib_dir() / "libcxx" / "modules";
    Path cppm_in = modules_dir / (std::string(module_name) + ".cppm.in");
    if (!cppm_in.is_regular_file()) return Path();

    auto in_content = read_file(cppm_in);
    if (!in_content) return Path();

    // Collect all .inc files (glob returns sorted results)
    auto incs = glob(modules_dir / std::string(module_name), "*.inc");
    if (incs.empty()) return Path();

    std::string inc_sources;
    for (const auto& inc : incs) {
        auto content = read_file(inc);
        if (content) {
            inc_sources += *content;
            inc_sources += "\n";
        }
    }

    // Replace the placeholder
    std::string cppm = *in_content;
    auto pos = cppm.find(placeholder);
    if (pos == std::string::npos) return Path();
    cppm.replace(pos, placeholder.size(), inc_sources);

    // Write to cache
    Path result = cache_dir / (std::string(module_name) + ".cppm");
    if (!write_file(result, cppm)) return Path();

    return result;
}

// Ensure both the std and std.compat PCMs are built in this project's output
// tree. Returns a map with "std" and "std.compat" entries on success, or an
// empty map if either source generation or compile fails. For Clang only.
export ModuleFileMap ensure_std_modules(
        const Toolchain& tc, const Path& project_out) {
    ModuleFileMap result;
    if (!tc.is_clang()) return result;

    // The project output cache keeps PCMs alongside other BMIs.
    Path pcm_cache = project_out / ".bmi" / ".std";
    pcm_cache.mkdir_recursive();

    // Generate both module interface sources.
    auto std_cppm = generate_standard_module_source(
        pcm_cache, "std", "@LIBCXX_MODULE_STD_INCLUDE_SOURCES@");
    if (std_cppm.string().empty()) return result;

    auto compat_cppm = generate_standard_module_source(
        pcm_cache, "std.compat",
        "@LIBCXX_MODULE_STD_COMPAT_INCLUDE_SOURCES@");
    if (compat_cppm.string().empty()) return result;

    // Build a cache key from compiler identity + source hashes + LLVM revision.
    auto cxx_args = cxx_prefix(tc);
    cxx_args.push_back("--version");
    auto ver = run_process(cxx_args, Path(), true);

    auto std_hash = SHA256::hex_file(std_cppm);
    auto compat_hash = SHA256::hex_file(compat_cppm);

    Path rev_file = find_lib_dir() / "libcxx" / "LLVM_REVISION";
    std::string revision;
    if (auto rev_content = read_file(rev_file))
        revision = *rev_content;

    std::string key_data = "std-modules-v2\n" +
        tc.cxx_path + "\n" +
        ver.stdout_output + "\n" +
        ver.stderr_output + "\n" +
        std_hash + "\n" +
        compat_hash + "\n" +
        revision;
    std::string key = SHA256::hex(key_data).substr(0, 16);

    Path std_pcm = pcm_cache / ("std-" + key + ".pcm");
    Path compat_pcm = pcm_cache / ("std.compat-" + key + ".pcm");

    bool std_ok = std_pcm.is_regular_file();
    bool compat_ok = compat_pcm.is_regular_file();

    Path libcxx_inc = find_lib_dir() / "libcxx" / "include";

    // Build std PCM if not cached.
    if (!std_ok) {
        std::println("   Preparing standard library module");

        std::vector<std::string> cmd;
        for (auto& a : cxx_prefix(tc)) cmd.push_back(a);
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

        auto build_result = run_process(cmd, Path(), true);
        if (!build_result.success()) {
            std::print(std::cerr, "{}", build_result.stderr_output);
            std::println(std::cerr, "bake: failed to pre-build std module");
            return result;
        }
        std_ok = true;
    }

    // Build std.compat PCM if not cached (depends on std PCM).
    if (!compat_ok) {
        std::vector<std::string> cmd;
        for (auto& a : cxx_prefix(tc)) cmd.push_back(a);
        cmd.push_back("-std=c++23");
        cmd.push_back("-stdlib=libc++");
        cmd.push_back("-nostdinc++");
        cmd.push_back("-I" + libcxx_inc.string());
        cmd.push_back("-Wno-reserved-module-identifier");
        cmd.push_back("-fmodule-file=std=" + std_pcm.string());
        cmd.push_back("-c");
        cmd.push_back(compat_cppm.string());
        cmd.push_back("--precompile");
        cmd.push_back("-o");
        cmd.push_back(compat_pcm.string());

        auto build_result = run_process(cmd, Path(), true);
        if (!build_result.success()) {
            std::print(std::cerr, "{}", build_result.stderr_output);
            std::println(std::cerr,
                         "bake: failed to pre-build std.compat module");
            return result;
        }
        compat_ok = true;
    }

    if (std_ok && compat_ok) {
        result["std"] = std_pcm;
        result["std.compat"] = compat_pcm;
    }
    return result;
}

// Determine the output library name based on type and platform
export std::string library_name(std::string_view base_name, PackageType type) {
    if (type == PackageType::SharedLib) {
#if defined(__APPLE__)
        return "lib" + std::string(base_name) + ".dylib";
#elif defined(_WIN32)
        return std::string(base_name) + ".dll";
#else
        return "lib" + std::string(base_name) + ".so";
#endif
    }
    if (type == PackageType::StaticLib) {
#if defined(_WIN32)
        return std::string(base_name) + ".lib";
#else
        return "lib" + std::string(base_name) + ".a";
#endif
    }
    // Executable
#if defined(_WIN32)
    return std::string(base_name) + ".exe";
#else
    return std::string(base_name);
#endif
}

} // namespace bake
