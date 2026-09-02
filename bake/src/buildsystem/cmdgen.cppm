export module bake.buildsystem.cmdgen;

import std;
import bake.util;
import bake.buildsystem.project;
import bake.toolchain.target;

// ============================================================
// bake.buildsystem.cmdgen — build intent → compiler invocation
//
// Translates moid declarations and manifest configuration into
// the parameters of a toolchain invocation: argv (make_*_command),
// profile flags, -D macros, and artifact names. Policy lives here;
// the toolchain only ever sees plain argv.
// ============================================================

namespace bake {

// ===== Compile configuration =====

export struct CompileConfig {
    Path source;
    Path output;           // .o file path
    std::string std_ver = "c++20";
    std::vector<Path> include_dirs;
    std::vector<std::pair<std::string, std::string>> defines;
    bool is_module_interface = false;
    Path bmi_output;       // BMI path (for module interfaces)
    std::vector<std::pair<std::string, Path>> module_deps;
    bool use_pic = false;
    std::vector<std::string> extra_flags;
};

export bool is_c_standard(std::string_view std_ver) {
    return (std_ver.starts_with("c") || std_ver.starts_with("gnu")) &&
           !std_ver.starts_with("c++") && !std_ver.starts_with("gnu++");
}

// ===== Link and archive commands =====

export struct LinkCommand {
    std::vector<Path> objects;
    std::vector<Path> libraries;
    Path output;
    MoidType type = MoidType::Executable;
    std::vector<std::string> system_libraries;
    std::vector<std::string> frameworks;
    std::vector<std::string> extra_flags;
    bool use_cxx_linker = true;
};

export struct ArchiveCommand {
    std::vector<Path> objects;
    Path output;
};

// ===== Command generation =====

export std::vector<std::string> make_compile_command(const TargetSpec& target,
                                                      const CompileConfig& cc) {
    std::vector<std::string> cmd;
    const bool compile_as_c = cc.source.is_c() && !cc.is_module_interface;

    cmd.push_back(bake_exe_path());
    cmd.push_back(compile_as_c ? "cc" : "c++");

    cmd.push_back("-c");
    cmd.push_back("-std=" + cc.std_ver);

    // Cross-compile target
    if (!target.is_native()) {
        cmd.push_back("-target");
        cmd.push_back(target.triple_with_version());
    }

    // libc++ for import std; / import std.compat;
    if (!compile_as_c) {
        for (const auto& [mod_name, _] : cc.module_deps) {
            if (mod_name == "std" || mod_name == "std.compat") {
                cmd.push_back("-Wno-reserved-module-identifier");
                break;
            }
        }
    }

    if (cc.use_pic) cmd.push_back("-fPIC");

    for (auto& flag : cc.extra_flags) cmd.push_back(flag);

    if (cc.is_module_interface) {
        cmd.push_back("-x");
        cmd.push_back("c++-module");
        if (!cc.bmi_output.string().empty())
            cmd.push_back("-fmodule-output=" + cc.bmi_output.string());
        else
            cmd.push_back("-fmodule-output");
    }

    for (auto& inc : cc.include_dirs) cmd.push_back("-I" + inc.string());

    for (auto& [name, value] : cc.defines) {
        cmd.push_back(value.empty() ? "-D" + name : "-D" + name + "=" + value);
    }

    if (!compile_as_c) {
        for (auto& [mod_name, bmi_path] : cc.module_deps)
            cmd.push_back("-fmodule-file=" + mod_name + "=" + bmi_path.string());
    }

    cmd.push_back(cc.source.string());
    cmd.push_back("-o");
    cmd.push_back(cc.output.string());

    return cmd;
}

export std::vector<std::string> make_link_command(const TargetSpec& target,
                                                   const LinkCommand& lc) {
    std::vector<std::string> cmd;
    cmd.push_back(bake_exe_path());
    cmd.push_back(lc.use_cxx_linker ? "c++" : "cc");

    // Cross-compile target
    if (!target.is_native()) {
        cmd.push_back("-target");
        cmd.push_back(target.triple_with_version());
    }

    if (lc.type == MoidType::Dylib) {
        cmd.push_back("-shared");
        cmd.push_back("-fPIC");
    }

    for (const auto& object : lc.objects)
        cmd.push_back(object.string());
    for (const auto& library : lc.libraries)
        cmd.push_back(library.string());

    for (const auto& library : lc.system_libraries) {
        if (library.find('/') != std::string::npos ||
            library.find('\\') != std::string::npos ||
            library.ends_with(".a") || library.ends_with(".lib") ||
            library.ends_with(".so") || library.ends_with(".dylib")) {
            cmd.push_back(library);
        } else {
            cmd.push_back("-l" + library);
        }
    }

    for (const auto& framework : lc.frameworks) {
        cmd.push_back("-framework");
        cmd.push_back(framework);
    }

    for (const auto& flag : lc.extra_flags)
        cmd.push_back(flag);

    cmd.push_back("-o");
    cmd.push_back(lc.output.string());

    return cmd;
}

export std::vector<std::string> make_archive_command(
        const TargetSpec& target, const ArchiveCommand& archive) {
    std::vector<std::string> cmd;
    cmd.push_back(bake_exe_path());
    cmd.push_back("ar");
    cmd.push_back("rcs");
    if (target.is_darwin())
        cmd.push_back("--darwin");
    cmd.push_back(archive.output.string());
    for (const auto& object : archive.objects)
        cmd.push_back(object.string());
    return cmd;
}

// ===== Profile → compiler flags =====

export struct ResolvedProfile {
    std::vector<std::string> compile_flags;
    std::vector<std::string> link_flags;
    std::vector<std::pair<std::string, std::string>> defines;
};

export ResolvedProfile resolve_profile_flags(const ProfileConfig& profile,
                                              bool is_release,
                                              const TargetSpec& target) {
    ResolvedProfile rp;

    // Optimization level
    if (profile.opt_size) {
        if (*profile.opt_size == "s") rp.compile_flags.push_back("-Os");
        else if (*profile.opt_size == "z") rp.compile_flags.push_back("-Oz");
    } else if (profile.opt_level) {
        rp.compile_flags.push_back("-O" + std::to_string(*profile.opt_level));
    }

    // Debug info
    if (profile.debug_kind) {
        rp.compile_flags.push_back("-g" + *profile.debug_kind);
    } else if (profile.debug && *profile.debug) {
        rp.compile_flags.push_back("-g");
    }

    // LTO
    if (profile.lto_kind) {
        rp.compile_flags.push_back("-flto=" + *profile.lto_kind);
    } else if (profile.lto && *profile.lto) {
        rp.compile_flags.push_back("-flto");
    }

    // Strip (link-time): drop debug info everywhere, and local symbols on
    // the formats where ld has a flag for it. MinGW keeps symbol tables
    // whole (its GNU-style -S already maps to the driver's strip_debug).
    if (profile.strip && *profile.strip) {
        rp.link_flags.push_back("-Wl,-S");
        if (!target.is_windows_gnu())
            rp.link_flags.push_back("-Wl,-x");
    }

    // Sanitizers
    if (!profile.sanitize.empty()) {
        std::string joined;
        for (std::size_t i = 0; i < profile.sanitize.size(); ++i) {
            if (i > 0) joined += ",";
            joined += profile.sanitize[i];
        }
        rp.compile_flags.push_back("-fsanitize=" + joined);
        rp.link_flags.push_back("-fsanitize=" + joined);
    }

    // Warning flags
    if (profile.warnings) {
        if (*profile.warnings == "all") {
            rp.compile_flags.push_back("-Wall");
        } else if (*profile.warnings == "extra") {
            rp.compile_flags.push_back("-Wall");
            rp.compile_flags.push_back("-Wextra");
        } else if (*profile.warnings == "error") {
            rp.compile_flags.push_back("-Wall");
            rp.compile_flags.push_back("-Wextra");
            rp.compile_flags.push_back("-Werror");
        }
        // "none" → no flags
    }

    // Release defines NDEBUG
    if (is_release) {
        rp.defines.emplace_back("NDEBUG", "1");
    }

    return rp;
}

// ===== Auto-macro generation =====
//
// BAKE_{MOID}_{OPTION} = 0/1  for each option.
// BAKE_{MOID}_NAME / VERSION / VERSION_MAJOR/MINOR/PATCH for package identity.

std::string normalize_macro_name(std::string_view name) {
    std::string result;
    result.reserve(name.size());
    for (char c : name) {
        if (c >= 'a' && c <= 'z') result += static_cast<char>(c - 'a' + 'A');
        else if (c == '-') result += '_';
        else result += c;
    }
    return result;
}

export std::vector<std::pair<std::string, std::string>>
generate_feature_macros(const std::string& moid_name,
                        const std::map<std::string, FeatureSpec>& features,
                        const std::vector<std::string>& active) {
    std::string prefix = "BAKE_" + normalize_macro_name(moid_name);
    std::set<std::string> active_set(active.begin(), active.end());
    std::vector<std::pair<std::string, std::string>> macros;
    for (const auto& [name, spec] : features) {
        (void)spec;
        macros.emplace_back(prefix + "_" + normalize_macro_name(name),
                            active_set.count(name) ? "1" : "0");
    }
    return macros;
}

export std::vector<std::pair<std::string, std::string>>
generate_package_macros(const std::string& moid_name,
                        const std::string& version) {
    std::string prefix = "BAKE_" + normalize_macro_name(moid_name);
    std::vector<std::pair<std::string, std::string>> macros;

    macros.emplace_back(prefix + "_NAME", "\"" + moid_name + "\"");
    macros.emplace_back(prefix + "_VERSION", "\"" + version + "\"");

    // Parse major.minor.patch
    int major = 0, minor = 0, patch = 0;
    {
        std::size_t pos = 0;
        auto parse_component = [&]() -> int {
            int val = 0;
            while (pos < version.size() && version[pos] >= '0' && version[pos] <= '9') {
                val = val * 10 + (version[pos] - '0');
                ++pos;
            }
            if (pos < version.size() && version[pos] == '.') ++pos;
            return val;
        };
        major = parse_component();
        minor = parse_component();
        patch = parse_component();
    }
    macros.emplace_back(prefix + "_VERSION_MAJOR", std::to_string(major));
    macros.emplace_back(prefix + "_VERSION_MINOR", std::to_string(minor));
    macros.emplace_back(prefix + "_VERSION_PATCH", std::to_string(patch));

    return macros;
}

export std::string library_name(std::string_view base_name, MoidType type,
                                const TargetSpec& target = {}) {
    bool target_windows = !target.is_native() && target.is_windows();

    if (type == MoidType::Dylib) {
        if (target_windows) return std::string(base_name) + ".dll";
#if defined(__APPLE__)
        return "lib" + std::string(base_name) + ".dylib";
#elif defined(_WIN32)
        return std::string(base_name) + ".dll";
#else
        return "lib" + std::string(base_name) + ".so";
#endif
    }
    if (type == MoidType::Lib) {
        // MinGW and ELF both use GNU convention: lib<name>.a
        return "lib" + std::string(base_name) + ".a";
    }
    if (target_windows) return std::string(base_name) + ".exe";
#if defined(_WIN32)
    return std::string(base_name) + ".exe";
#else
    return std::string(base_name);
#endif
}

} // namespace bake
