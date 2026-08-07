module;

#include <unistd.h>

export module bake.compiler;

import std;
import bake.util;
import bake.project;

// ============================================================
// bake.compiler — toolchain detection, compile/link commands
//
// bake always uses its own embedded Clang/LLD (in-process).
// There is no external compiler — Toolchain just holds the
// path to the bake binary and optional cross-compile target.
// ============================================================

namespace bake {

// ===== Cross-compilation target =====

export struct TargetSpec {
    std::string arch;   // "x86_64" | "aarch64" | "" (native)
    std::string os;     // "linux" | "macos" | "" (native)
    std::string abi;    // "musl" | "gnu" | "" (native)

    bool is_native() const { return arch.empty(); }
    bool is_linux_musl() const { return os == "linux" && abi == "musl"; }
    bool is_linux() const { return os == "linux"; }

    std::string triple() const {
        if (is_native()) return "";
        return arch + "-" + os + "-" + abi;
    }

    // musl internal arch directory name (matches musl/arch/<name>/).
    std::string musl_arch_dir() const { return arch; }
};

// Parse "x86_64-linux-musl" → TargetSpec.
// "" / "native" / "host" → native (all fields empty).
export TargetSpec parse_target(std::string_view spec) {
    TargetSpec t;
    if (spec.empty() || spec == "native" || spec == "host") return t;

    auto pos1 = spec.find('-');
    if (pos1 == std::string_view::npos) return t;
    t.arch = std::string(spec.substr(0, pos1));

    auto rest = spec.substr(pos1 + 1);
    auto pos2 = rest.find('-');
    if (pos2 == std::string_view::npos) {
        t.os = std::string(rest);
        return t;
    }
    t.os = std::string(rest.substr(0, pos2));
    t.abi = std::string(rest.substr(pos2 + 1));
    return t;
}

// ===== Toolchain =====

export struct Toolchain {
    std::string exe_path;   // path to the bake binary (used as cc/c++ driver)
    std::string ar_path;    // archiver (system ar for now)
    TargetSpec target;      // cross-compile target (default: native)

    static Toolchain detect() {
        Toolchain tc;
        // When running inside build_app (linked to core), get_self_exe_path()
        // returns build_app's path, not bake's. Use BAKE_EXE env var if set.
        if (const char* bake_exe = std::getenv("BAKE_EXE"))
            tc.exe_path = bake_exe;
        else
            tc.exe_path = get_self_exe_path();
        if (tc.exe_path.empty()) tc.exe_path = "bake";

        if (auto p = find_in_path("ar"))
            tc.ar_path = p->string();
        else
            tc.ar_path = "ar";

        return tc;
    }

    std::string cxx() const { return exe_path; }
    std::string cc() const { return exe_path; }
};

// argv prefix for C++ driver invocation: ["bake", "c++"]
export inline std::vector<std::string> cxx_prefix(const Toolchain& tc) {
    return {tc.exe_path, "c++"};
}

// argv prefix for C driver invocation: ["bake", "cc"]
export inline std::vector<std::string> cc_prefix(const Toolchain& tc) {
    return {tc.exe_path, "cc"};
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
    bool use_cxx_linker = true;
};

export struct ArchiveCommand {
    std::vector<Path> objects;
    Path output;
};

// ===== Command generation =====

export std::vector<std::string> make_compile_command(const Toolchain& tc,
                                                      const CompileConfig& cc) {
    std::vector<std::string> cmd;
    const bool compile_as_c = cc.source.is_c() && !cc.is_module_interface;

    cmd.push_back(tc.exe_path);
    cmd.push_back(compile_as_c ? "cc" : "c++");

    cmd.push_back("-c");
    cmd.push_back("-std=" + cc.std_ver);

    // Cross-compile target
    if (!tc.target.is_native()) {
        cmd.push_back("-target");
        cmd.push_back(tc.target.triple());
    }

    // libc++ for import std; / import std.compat;
    if (!compile_as_c) {
        for (const auto& [mod_name, _] : cc.module_deps) {
            if (mod_name == "std" || mod_name == "std.compat") {
                cmd.push_back("-stdlib=libc++");
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

export std::vector<std::string> make_link_command(const Toolchain& tc,
                                                   const LinkCommand& lc) {
    std::vector<std::string> cmd;
    cmd.push_back(tc.exe_path);
    cmd.push_back(lc.use_cxx_linker ? "c++" : "cc");

    // Cross-compile target
    if (!tc.target.is_native()) {
        cmd.push_back("-target");
        cmd.push_back(tc.target.triple());
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

    cmd.push_back("-o");
    cmd.push_back(lc.output.string());

    return cmd;
}

export std::vector<std::string> make_archive_command(
        const Toolchain& tc, const ArchiveCommand& archive) {
    std::vector<std::string> cmd;
    cmd.push_back(tc.ar_path);
    cmd.push_back("rcs");
    cmd.push_back(archive.output.string());
    for (const auto& object : archive.objects)
        cmd.push_back(object.string());
    return cmd;
}

// ===== Standard module (std / std.compat) PCM management =====

export using ModuleFileMap = std::map<std::string, Path>;

static Path generate_standard_module_source(
        const Path& cache_dir,
        std::string_view module_name,
        std::string_view placeholder) {
    Path result = cache_dir / (std::string(module_name) + ".cppm");
    if (result.is_regular_file()) return result;

    Path modules_dir = find_lib_dir() / "libcxx" / "modules";
    Path cppm_in = modules_dir / (std::string(module_name) + ".cppm.in");
    if (!cppm_in.is_regular_file()) return Path();

    auto in_content = read_file(cppm_in);
    if (!in_content) return Path();

    auto incs = glob(modules_dir / std::string(module_name), "*.inc");
    if (incs.empty()) return Path();

    std::string inc_sources;
    for (const auto& inc : incs) {
        if (auto content = read_file(inc)) {
            inc_sources += *content;
            inc_sources += "\n";
        }
    }

    std::string cppm = *in_content;
    auto pos = cppm.find(placeholder);
    if (pos == std::string::npos) return Path();
    cppm.replace(pos, placeholder.size(), inc_sources);

    if (!write_file(result, cppm)) return Path();
    return result;
}

// Capture compiler identity + version + target triple as a stable string.
// Cached to a file so we avoid spawning the compiler on every invocation.
static std::string compiler_identity_block(const Toolchain& tc) {
    namespace fs = std::filesystem;

    // Include target triple in cache identity so cross-compile keys differ.
    std::string target_suffix = tc.target.is_native() ? "" : "\n" + tc.target.triple();

    Path identity_cache = get_toolchain_cache_root() / ".compiler-identity";
    Path compiler_bin(tc.exe_path);
    if (identity_cache.is_regular_file() && compiler_bin.is_regular_file()) {
        if (fs::last_write_time(compiler_bin.fs()) <=
            fs::last_write_time(identity_cache.fs())) {
            if (auto cached = read_file(identity_cache)) {
                // Invalidate cache if target changed.
                if (cached->ends_with(target_suffix + "\n") ||
                    (target_suffix.empty() && !contains(*cached, "-linux-")))
                    return *cached;
            }
        }
    }

    auto prefix = cxx_prefix(tc);

    auto ver_args = prefix;
    ver_args.push_back("--version");
    auto ver = run_process(ver_args, Path(), true);

    auto triple_args = prefix;
    triple_args.push_back("-dumpmachine");
    auto triple = run_process(triple_args, Path(), true);

    std::string block = tc.exe_path + "\n" +
                        ver.stdout_output + "\n" +
                        ver.stderr_output + "\n" +
                        triple.stdout_output + "\n" +
                        target_suffix + "\n";

    write_file(identity_cache, block);
    return block;
}

struct ToolchainCacheInfo {
    std::string key;
    Path dir;
};

static ToolchainCacheInfo std_module_cache_info(const Toolchain& tc) {
    Path gen_dir = get_toolchain_cache_root() / ".gen";
    gen_dir.mkdir_recursive();

    auto std_cppm = generate_standard_module_source(
        gen_dir, "std", "@LIBCXX_MODULE_STD_INCLUDE_SOURCES@");
    auto compat_cppm = generate_standard_module_source(
        gen_dir, "std.compat",
        "@LIBCXX_MODULE_STD_COMPAT_INCLUDE_SOURCES@");

    std::string std_hash, compat_hash;
    if (!std_cppm.string().empty())
        std_hash = SHA256::hex_file(std_cppm);
    if (!compat_cppm.string().empty())
        compat_hash = SHA256::hex_file(compat_cppm);

    Path rev_file = find_lib_dir() / "libcxx" / "LLVM_REVISION";
    std::string revision;
    if (auto rev_content = read_file(rev_file))
        revision = *rev_content;

    std::string key_data = "std-modules-v3\n" +
        compiler_identity_block(tc) +
        std_hash + "\n" +
        compat_hash + "\n" +
        revision;

    std::string key = SHA256::hex(key_data).substr(0, 16);
    Path dir = get_toolchain_cache_root() / key;
    return {key, dir};
}

static bool atomic_compile_pcm(
        const std::vector<std::string>& compile_cmd,
        const Path& dest) {
    if (dest.is_regular_file()) return true;

    Path parent = dest.parent();
    parent.mkdir_recursive();

    std::string suffix = "." + std::to_string(getpid()) + ".tmp";
    Path tmp = Path(dest.string() + suffix);

    std::vector<std::string> cmd = compile_cmd;
    for (auto& arg : cmd) {
        if (arg == dest.string()) arg = tmp.string();
    }

    auto result = run_process(cmd, Path(), true);
    if (!result.success()) {
        std::print(std::cerr, "{}", result.stderr_output);
        Path(tmp).remove();
        return false;
    }

    std::error_code ec;
    std::filesystem::rename(tmp.string(), dest.string(), ec);
    if (ec) {
        Path(tmp).remove();
        return dest.is_regular_file();
    }
    return true;
}

export ModuleFileMap ensure_std_modules(
        const Toolchain& tc, const Path& /*project_out*/) {
    ModuleFileMap result;

    auto info = std_module_cache_info(tc);
    if (info.key.empty()) return result;

    Path std_dir = info.dir / "std";
    Path std_pcm = std_dir / "std.pcm";
    Path compat_pcm = std_dir / "std.compat.pcm";

    Path gen_dir = get_toolchain_cache_root() / ".gen";
    Path std_src = gen_dir / "std.cppm";
    Path compat_src = gen_dir / "std.compat.cppm";

    Path libcxx_inc = find_lib_dir() / "libcxx" / "include";

    // Target flag for cross-compile std module pre-compilation.
    std::vector<std::string> target_flags;
    if (!tc.target.is_native()) {
        target_flags.push_back("-target");
        target_flags.push_back(tc.target.triple());
    }

    if (!std_pcm.is_regular_file()) {
        std::println("   Preparing standard library module");

        std::vector<std::string> cmd = cxx_prefix(tc);
        cmd.push_back("-std=c++23");
        cmd.push_back("-stdlib=libc++");
        cmd.push_back("-nostdinc++");
        cmd.push_back("-isystem");
        cmd.push_back((find_lib_dir() / "libcxx" / "include").string());
        cmd.push_back("-isystem");
        cmd.push_back((find_lib_dir() / "include").string());
        cmd.push_back("-Wno-reserved-module-identifier");
        for (auto& f : target_flags) cmd.push_back(f);
        cmd.push_back("-c");
        cmd.push_back(std_src.string());
        cmd.push_back("--precompile");
        cmd.push_back("-o");
        cmd.push_back(std_pcm.string());

        if (!atomic_compile_pcm(cmd, std_pcm)) {
            std::println(std::cerr, "bake: failed to pre-build std module");
            return result;
        }
    }

    if (!compat_pcm.is_regular_file()) {
        std::vector<std::string> cmd = cxx_prefix(tc);
        cmd.push_back("-std=c++23");
        cmd.push_back("-stdlib=libc++");
        cmd.push_back("-nostdinc++");
        cmd.push_back("-isystem");
        cmd.push_back(libcxx_inc.string());
        cmd.push_back("-isystem");
        cmd.push_back((find_lib_dir() / "include").string());
        cmd.push_back("-Wno-reserved-module-identifier");
        cmd.push_back("-fmodule-file=std=" + std_pcm.string());
        for (auto& f : target_flags) cmd.push_back(f);
        cmd.push_back("-c");
        cmd.push_back(compat_src.string());
        cmd.push_back("--precompile");
        cmd.push_back("-o");
        cmd.push_back(compat_pcm.string());

        if (!atomic_compile_pcm(cmd, compat_pcm)) {
            std::println(std::cerr,
                         "bake: failed to pre-build std.compat module");
            return result;
        }
    }

    result["std"] = std_pcm;
    result["std.compat"] = compat_pcm;
    return result;
}

export ToolchainCacheInfo bake_build_cache_info(
        const Toolchain& tc, const Path& wrapper_source) {
    auto base = std_module_cache_info(tc);

    std::string wrapper_hash;
    if (!wrapper_source.string().empty() && wrapper_source.is_regular_file())
        wrapper_hash = SHA256::hex_file(wrapper_source);

    std::string key_data = "bake.build-v1\n" +
        base.key + "\n" +
        wrapper_hash;
    std::string key = SHA256::hex(key_data).substr(0, 16);
    Path dir = get_toolchain_cache_root() / key;
    return {key, dir};
}

export std::string library_name(std::string_view base_name, MoidType type) {
    if (type == MoidType::Dylib) {
#if defined(__APPLE__)
        return "lib" + std::string(base_name) + ".dylib";
#elif defined(_WIN32)
        return std::string(base_name) + ".dll";
#else
        return "lib" + std::string(base_name) + ".so";
#endif
    }
    if (type == MoidType::Lib) {
#if defined(_WIN32)
        return std::string(base_name) + ".lib";
#else
        return "lib" + std::string(base_name) + ".a";
#endif
    }
#if defined(_WIN32)
    return std::string(base_name) + ".exe";
#else
    return std::string(base_name);
#endif
}

} // namespace bake
