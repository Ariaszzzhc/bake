module;

#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <optional>
#include <cstdio>
#include <cstdlib>

export module bake.compiler;

import bake.util;
import bake.project;

// ============================================================
// bake.compiler — toolchain detection, compile/link commands
// ============================================================

namespace bake {

export enum class CompilerKind {
    Clang,
    Gcc,
    AppleClang,
    Unknown,
};

export struct Toolchain {
    CompilerKind kind = CompilerKind::Unknown;
    std::string cxx_path;       // C++ compiler
    std::string cc_path;        // C compiler
    std::string ar_path;        // archiver
    std::string scanner_path;   // clang-scan-deps (Clang only)

    bool has_scanner() const { return !scanner_path.empty(); }
    bool is_clang() const { return kind == CompilerKind::Clang || kind == CompilerKind::AppleClang; }
    bool is_gcc() const { return kind == CompilerKind::Gcc; }

    static Toolchain detect() {
        Toolchain tc;

        // 1. Check CXX environment variable
        if (const char* cxx_env = std::getenv("CXX")) {
            if (Path(cxx_env).is_regular_file() || run_process({cxx_env, "--version"}, Path(), true).success()) {
                tc.cxx_path = cxx_env;
            }
        }

        // 2. Prefer clang++ on PATH (better module support than Apple Clang's c++)
        if (tc.cxx_path.empty()) {
            auto which = run_process({"sh", "-c", "command -v clang++"}, Path(), true);
            if (which.success()) {
                tc.cxx_path = trim(which.stdout_output);
            }
        }

        // 3. Fall back to c++
        if (tc.cxx_path.empty()) {
            auto which = run_process({"sh", "-c", "command -v c++"}, Path(), true);
            if (which.success()) {
                tc.cxx_path = trim(which.stdout_output);
            }
        }
        if (tc.cxx_path.empty()) {
            tc.cxx_path = "c++";
        }

        // Find cc compiler
        if (const char* cc_env = std::getenv("CC")) {
            tc.cc_path = cc_env;
        } else {
            auto which_cc = run_process({"sh", "-c", "command -v clang || command -v cc"}, Path(), true);
            if (which_cc.success()) {
                tc.cc_path = trim(which_cc.stdout_output);
            }
        }
        if (tc.cc_path.empty()) tc.cc_path = "cc";

        // Find ar
        auto which_ar = run_process({"sh", "-c", "command -v ar"}, Path(), true);
        if (which_ar.success()) {
            tc.ar_path = trim(which_ar.stdout_output);
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
                auto which_scan = run_process({"sh", "-c", "command -v clang-scan-deps"}, Path(), true);
                if (which_scan.success()) {
                    tc.scanner_path = trim(which_scan.stdout_output);
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
            default:                        return "unknown";
        }
    }
};

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
};

// ===== Link configuration =====

export struct LinkConfig {
    std::vector<Path> inputs;   // .o files
    Path output;                // output path
    PackageType type = PackageType::Executable;
    std::vector<std::string> link_libs;  // -l flags
};

// ===== Command generation =====

export std::vector<std::string> make_compile_command(const Toolchain& tc,
                                                      const CompileConfig& cc) {
    std::vector<std::string> cmd;
    cmd.push_back(tc.cxx_path);

    // Mode
    cmd.push_back("-c");

    // Standard
    cmd.push_back("-std=" + cc.std_ver);

    // PIC
    if (cc.use_pic) {
        cmd.push_back("-fPIC");
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

    // Module dependencies (BMI references for consumers)
    if (!cc.is_module_interface && !cc.module_deps.empty()) {
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
    cmd.push_back(tc.cxx_path);

    if (lc.type == PackageType::SharedLib) {
        cmd.push_back("-shared");
        cmd.push_back("-fPIC");
    }

    // Input files
    for (auto& input : lc.inputs) {
        cmd.push_back(input.string());
    }

    // Link libraries
    for (auto& lib : lc.link_libs) {
        cmd.push_back("-l" + lib);
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

// Determine the output library name based on type and platform
export std::string library_name(std::string_view base_name, PackageType type) {
#if defined(__APPLE__)
    if (type == PackageType::SharedLib) return "lib" + std::string(base_name) + ".dylib";
#elif defined(__linux__)
    if (type == PackageType::SharedLib) return "lib" + std::string(base_name) + ".so";
#else
    if (type == PackageType::SharedLib) return std::string(base_name) + ".dll";
#endif
    if (type == PackageType::StaticLib) return "lib" + std::string(base_name) + ".a";
    return std::string(base_name);
}

} // namespace bake
