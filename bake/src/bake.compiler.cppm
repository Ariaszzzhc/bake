module;

#include <unistd.h>

export module bake.compiler;

import std;
import bake.util;
import bake.project;
import bake.llvm;

// ============================================================
// bake.compiler — toolchain detection, compile/link commands
//
// bake always uses its own embedded Clang/LLD (in-process).
// There is no external compiler — Toolchain just holds the
// path to the bake binary and optional cross-compile target.
// ============================================================

namespace bake {

// ===== Cross-compilation target =====
//
// TargetSpec stores a canonical triple string in arch-os-abi format
// (no vendor field), matching the multiarch directory convention used
// by libc header paths and cache keys. When passed to Clang via
// -target, LLVM's normalize() inserts the implicit "unknown" vendor
// automatically — the canonical form is always recoverable.
//
// The triple string is the single source of truth. All property
// queries (is_darwin, is_linux_musl, arch, …) derive from it.

export struct TargetSpec {
    std::string triple_;  // "arch-os[-abi]" (empty when native)
    bool native_ = true;  // true = compiling for host (no -target flag)

    bool is_native() const { return native_; }

    bool is_darwin() const {
        return triple_.contains("macos") || triple_.contains("darwin")
            || triple_.contains("apple");
    }

    bool is_linux() const { return triple_.contains("linux"); }

    bool is_linux_musl() const {
        return triple_.contains("linux") && triple_.contains("musl");
    }

    bool is_windows() const {
        return triple_.contains("windows");
    }

    bool is_windows_gnu() const {
        return triple_.contains("windows") && triple_.contains("gnu");
    }

    // Architecture component — first segment of the triple.
    std::string arch() const {
        auto pos = triple_.find('-');
        return pos == std::string::npos ? triple_ : triple_.substr(0, pos);
    }

    // Returns the triple string for the -target flag, or "" for native.
    std::string triple() const { return native_ ? "" : triple_; }
};

// Detect host platform. This is the ONLY place with #ifdef for host detection.
export TargetSpec detect_host_target() {
    TargetSpec t;
    t.native_ = true;
#if defined(__APPLE__) && defined(__aarch64__)
    t.triple_ = "aarch64-macos";
#elif defined(__APPLE__) && defined(__x86_64__)
    t.triple_ = "x86_64-macos";
#elif defined(__linux__) && defined(__aarch64__)
    t.triple_ = "aarch64-linux";
#elif defined(__linux__) && defined(__x86_64__)
    t.triple_ = "x86_64-linux";
#else
    t.triple_ = "unknown";
#endif
    return t;
}

// Parse a user-supplied target spec into a canonical arch-os-abi triple.
// Handles both 3-component (Zig-style "aarch64-linux-musl") and
// 4-component (LLVM-style "aarch64-unknown-linux-musl") inputs by
// discarding the vendor field. Architecture aliases are normalized
// (arm64→aarch64, amd64→x86_64) to match LLVM/Clang canonical names.
export TargetSpec parse_target(std::string_view spec) {
    TargetSpec t;
    if (spec.empty() || spec == "native" || spec == "host") return t;
    t.native_ = false;

    // Split on '-', discarding known vendor components.
    static constexpr std::string_view vendors[] = {
        "unknown", "apple", "pc", "squeakboard", "wrs", "img", "myriad"
    };

    std::vector<std::string> kept;
    size_t start = 0;
    for (size_t i = 0; i <= spec.size(); ++i) {
        if (i == spec.size() || spec[i] == '-') {
            if (i > start) {
                std::string_view comp = spec.substr(start, i - start);
                bool is_vendor = false;
                for (auto v : vendors)
                    if (comp == v) { is_vendor = true; break; }
                if (!is_vendor) kept.emplace_back(comp);
            }
            start = i + 1;
        }
    }

    if (kept.empty()) return t;

    // Normalize architecture aliases to LLVM canonical names.
    if (kept[0] == "arm64")  kept[0] = "aarch64";
    if (kept[0] == "amd64")  kept[0] = "x86_64";

    // Normalize MinGW triples: x86_64-w64-mingw32 → x86_64-windows-gnu.
    // LLVM canonical form is <arch>-windows-gnu; the legacy w64-mingw32
    // suffix is the historical MinGW-w64 convention.
    for (size_t i = 1; i < kept.size(); ++i) {
        if (kept[i].starts_with("mingw32")) {
            kept.erase(kept.begin() + 1, kept.end());
            kept.push_back("windows-gnu");
            break;
        }
    }

    t.triple_ = kept[0];
    for (size_t i = 1; i < kept.size(); ++i)
        t.triple_ += "-" + kept[i];

    return t;
}

// ===== Toolchain =====

export struct Toolchain {
    std::string exe_path;   // path to the bake binary (used as cc/c++/ar driver)
    TargetSpec target;      // cross-compile target (default: native)

    static Toolchain detect() {
        Toolchain tc;
        if (const char* bake_exe = std::getenv("BAKE_EXE"))
            tc.exe_path = bake_exe;
        else
            tc.exe_path = get_self_exe_path();
        if (tc.exe_path.empty()) tc.exe_path = "bake";

        tc.target = detect_host_target();
        return tc;
    }

    std::string cxx() const { return exe_path; }
    std::string cc() const { return exe_path; }
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
    cmd.push_back(tc.exe_path);
    cmd.push_back("ar");
    cmd.push_back("rcs");
    if (tc.target.is_darwin())
        cmd.push_back("--darwin");
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

    // Per-target cache file to avoid native/cross thrashing.
    std::string id_name = ".compiler-identity";
    if (!tc.target.is_native())
        id_name += "-" + tc.target.triple();

    Path identity_cache = get_toolchain_cache_root() / id_name;
    Path compiler_bin(tc.exe_path);
    if (identity_cache.is_regular_file() && compiler_bin.is_regular_file()) {
        if (fs::last_write_time(compiler_bin.fs()) <=
            fs::last_write_time(identity_cache.fs())) {
            if (auto cached = read_file(identity_cache))
                return *cached;
        }
    }

    std::vector<std::string> prefix = {tc.exe_path, "c++"};

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

    // Select libc++ config_site based on target: mingw-config for windows-gnu,
    // cross-config for other cross-compile targets.
    std::string config_subdir = tc.target.is_windows_gnu()
        ? "mingw-config" : "cross-config";

    if (!std_pcm.is_regular_file()) {
        std::println("   Preparing standard library module");

        std::vector<std::string> cmd = {tc.exe_path, "c++"};
        cmd.push_back("-std=c++23");
        cmd.push_back("-stdlib=libc++");
        cmd.push_back("-nostdinc++");

        // Cross-compile: -target + cross __config_site before libc++ headers.
        if (!tc.target.is_native()) {
            cmd.push_back("-target");
            cmd.push_back(tc.target.triple());
            cmd.push_back("-isystem");
            cmd.push_back((find_lib_dir() / "libcxx" / config_subdir).string());
        }

        cmd.push_back("-isystem");
        cmd.push_back(libcxx_inc.string());
        cmd.push_back("-isystem");
        cmd.push_back((find_lib_dir() / "include").string());
        cmd.push_back("-Wno-reserved-module-identifier");
        cmd.push_back("-Wno-unused-command-line-argument");
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
        std::vector<std::string> cmd = {tc.exe_path, "c++"};
        cmd.push_back("-std=c++23");
        cmd.push_back("-stdlib=libc++");
        cmd.push_back("-nostdinc++");

        if (!tc.target.is_native()) {
            cmd.push_back("-target");
            cmd.push_back(tc.target.triple());
            cmd.push_back("-isystem");
            cmd.push_back((find_lib_dir() / "libcxx" / config_subdir).string());
        }

        cmd.push_back("-isystem");
        cmd.push_back(libcxx_inc.string());
        cmd.push_back("-isystem");
        cmd.push_back((find_lib_dir() / "include").string());
        cmd.push_back("-Wno-reserved-module-identifier");
        cmd.push_back("-Wno-unused-command-line-argument");
        cmd.push_back("-fmodule-file=std=" + std_pcm.string());
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

// ===== In-process archive writer (replaces system ar) =====
//
// bake embeds LLVM's archive writer, so it never depends on external `ar`.

static bool write_archive(const Path& archive_path,
                          const std::vector<Path>& members,
                          bool is_darwin, bool is_windows = false) {
    std::vector<std::string> paths;
    std::vector<const char *> cstrs;
    paths.reserve(members.size());
    cstrs.reserve(members.size());
    for (auto& m : members) {
        paths.push_back(m.string());
        cstrs.push_back(paths.back().c_str());
    }
    int kind = is_darwin ? 2 : is_windows ? 3 : 0;  // DARWIN, COFF, GNU
    return bake_ar_write(archive_path.string().c_str(),
                         cstrs.data(), cstrs.size(), kind) == 0;
}

// ===== Runtime object management =====
//
// bake vendors libc++, libc++abi, libunwind, compiler-rt, and musl sources
// alongside the binary. These are compiled on first use (or when the cache
// is invalidated) and cached globally keyed by target triple.
//
// Architecture (three layers):
//   1. resolve_libc_family(target) → LibcFamily  (single dispatch point)
//   2. prepare_runtime(tc, ...) → RuntimeArtifacts (calls ensure_* per family)
//   3. bakeExecuteJob consumes RuntimeArtifacts  (target-agnostic assembly)
//
// Adding a new libc family (e.g. Gnu, Mingw) only requires:
//   - A new enum value in LibcFamily
//   - A new case in resolve_libc_family()
//   - A new case in prepare_runtime()
//   - The family's ensure_* builder function
// The linker code (bake_clang_driver.cpp) stays untouched.
//
// C vs C++ runtime: `bake cc` (C mode) only needs libc/compiler-rt.
// `bake c++` (C++ mode) additionally needs libc++/libc++abi/libunwind.
//
// All functions below are called from the in-process Clang driver
// (bake_clang_driver.cpp), making `bake cc/c++ -target <triple>` a
// complete drop-in cross-compiler with no external dependencies.

// ── Libc family: the single dispatch point for runtime selection ──

export enum class LibcFamily {
    Musl,     // built from vendored source
    Darwin,   // libSystem via vendored .tbd (cross) or system SDK (native)
    Windows,  // MinGW-w64 (built from vendored source)
    None,     // freestanding (no libc)
};

export enum class LinkMode {
    Static,      // -static
    StaticPie,   // -static-pie
    Dynamic,     // default or -shared
};

export enum class DarwinSdkLayout {
    SystemSdk,   // native: use xcrun SDK for libSystem + C headers + frameworks
    Vendored,    // cross: use vendored libSystem.tbd + headers, no frameworks
};

export struct RuntimeArtifacts {
    // C runtime
    Path crt_entry;      // crt1.o / rcrt1.o / Scrt1.o (musl only; empty for darwin)
    Path libc;           // libc.a (musl); empty for darwin (uses libSystem.tbd)
    Path compiler_rt;    // libcompiler_rt.a (all targets)

    // C++ runtime (populated only when is_cxx)
    Path libcxx;
    Path libcxxabi;
    Path libunwind;      // musl only; darwin uses libSystem's unwind

    // darwin link helpers
    std::vector<std::string> link_dirs;        // -L paths (SDK or vendored)
    std::vector<std::string> framework_dirs;   // -F paths
    std::string macos_deployment_target;       // -mmacosx-version-min value

    // windows (mingw) link helpers
    std::vector<std::string> always_link_libs; // system libs to inject
    std::string mingw_import_dir;              // path to generated import libs
};

export LibcFamily resolve_libc_family(const TargetSpec& target) {
    if (target.is_linux_musl())  return LibcFamily::Musl;
    if (target.is_windows_gnu()) return LibcFamily::Windows;
    if (target.is_darwin())      return LibcFamily::Darwin;
    return LibcFamily::None;
}

export DarwinSdkLayout resolve_darwin_sdk(const TargetSpec& target) {
    // Native darwin → try system SDK; cross → vendored
    auto host = detect_host_target();
    if (host.is_darwin() && target.is_darwin())
        return DarwinSdkLayout::SystemSdk;
    return DarwinSdkLayout::Vendored;
}

export LinkMode parse_link_mode(const std::vector<std::string>& args) {
    bool is_shared = false;
    bool is_static_pie = false;
    for (auto& a : args) {
        if (a == "-shared" || a == "-Bshareable") is_shared = true;
        if (a == "-static-pie") is_static_pie = true;
    }
    if (is_static_pie) return LinkMode::StaticPie;
    if (is_shared)     return LinkMode::Dynamic;
    return LinkMode::Static;
}

// prepare_runtime() is defined after all ensure_* functions (below).

// ── Source file lists (curated from upstream libcxx/libunwind) ──

static const char* libcxx_base_files[] = {
    "algorithm.cpp", "any.cpp", "bind.cpp", "call_once.cpp", "charconv.cpp",
    "chrono.cpp", "error_category.cpp", "exception.cpp", "expected.cpp",
    "filesystem/directory_entry.cpp", "filesystem/directory_iterator.cpp",
    "filesystem/filesystem_clock.cpp", "filesystem/filesystem_error.cpp",
    "filesystem/int128_builtins.cpp", "filesystem/operations.cpp",
    "filesystem/path.cpp", "fstream.cpp", "functional.cpp", "hash.cpp",
    "ios.cpp", "ios.instantiations.cpp", "iostream.cpp", "locale.cpp",
    "memory.cpp", "memory_resource.cpp", "new.cpp", "new_handler.cpp",
    "new_helpers.cpp", "optional.cpp", "ostream.cpp", "print.cpp",
    "random.cpp", "random_shuffle.cpp", "regex.cpp",
    "ryu/d2fixed.cpp", "ryu/d2s.cpp", "ryu/f2s.cpp",
    "stdexcept.cpp", "string.cpp", "strstream.cpp", "system_error.cpp",
    "typeinfo.cpp", "valarray.cpp", "variant.cpp", "vector.cpp",
    "verbose_abort.cpp", "mutex_destructor.cpp", "condition_variable_destructor.cpp",
};
static const char* libcxx_thread_files[] = {
    "atomic.cpp", "barrier.cpp", "condition_variable.cpp", "future.cpp",
    "mutex.cpp", "shared_mutex.cpp", "thread.cpp",
};
static const char* libcxxabi_files[] = {
    "abort_message.cpp", "cxa_aux_runtime.cpp", "cxa_default_handlers.cpp",
    "cxa_demangle.cpp", "cxa_exception.cpp", "cxa_exception_storage.cpp",
    "cxa_guard.cpp", "cxa_handlers.cpp", "cxa_noexception.cpp",
    "cxa_personality.cpp", "cxa_thread_atexit.cpp", "cxa_vector.cpp",
    "cxa_virtual.cpp", "fallback_malloc.cpp", "private_typeinfo.cpp",
    "stdlib_exception.cpp", "stdlib_new_delete.cpp", "stdlib_stdexcept.cpp",
    "stdlib_typeinfo.cpp",
};
static const char* libunwind_c_files[] = {
    "UnwindLevel1.c", "UnwindLevel1-gcc-ext.c",
    "Unwind-sjlj.c", "Unwind-wasm.c",
};
static const char* libunwind_cpp_files[] = {
    "libunwind.cpp", "Unwind-EHABI.cpp", "Unwind-seh.cpp",
};
static const char* libunwind_asm_files[] = {
    "UnwindRegistersRestore.S", "UnwindRegistersSave.S",
};

// ── Unified C++ runtime: libc++ + libc++abi (+ libunwind for musl) ──
//
// One function for ALL targets (darwin, musl). Cache key = target triple.
// Returns .a archives so the linker only pulls needed objects.
// libunwind is only built for musl (darwin uses libSystem's unwind).

export struct CxxRuntime {
    Path libcxx_a;
    Path libcxxabi_a;
    Path libunwind_a;  // empty for darwin
};

export CxxRuntime ensure_cxx_runtime(const Toolchain& tc) {
    CxxRuntime result;
    Path lib = find_lib_dir();
    if (lib.string().empty()) return result;

    // Cache per target triple (same result regardless of host).
    std::string key = tc.target.is_native() ? "host" : tc.target.triple();
    Path cache_dir = get_cache_dir().parent() / "cxx-runtime" / key;

    result.libcxx_a    = cache_dir / "libc++.a";
    result.libcxxabi_a = cache_dir / "libc++abi.a";

    bool needs_libunwind = tc.target.is_linux_musl() || tc.target.is_windows();
    if (needs_libunwind)
        result.libunwind_a = cache_dir / "libunwind.a";

    Path sentinel = cache_dir / ".done";
    auto cached = [&]() {
        if (!sentinel.is_regular_file()) return false;
        if (!result.libcxx_a.is_regular_file()) return false;
        if (!result.libcxxabi_a.is_regular_file()) return false;
        if (needs_libunwind && !result.libunwind_a.is_regular_file()) return false;
        return true;
    };
    if (cached()) return result;

    cache_dir.mkdir_recursive();
    std::println("   Compiling C++ runtime for {} (cached)", key);

    auto libcxxabi_inc = lib.string() + "/libcxxabi/include";
    auto libcxx_src    = lib.string() + "/libcxx/src";
    auto libcxxabi_src = lib.string() + "/libcxxabi/src";
    auto libcxx_libc   = lib.string() + "/libcxx/libc";
    auto libunwind_inc = lib.string() + "/libunwind/include";
    auto libunwind_src = lib.string() + "/libunwind/src";

    // -target flag: always add when non-native (identical for all targets).
    std::vector<std::string> target_flag;
    if (!tc.target.is_native()) {
        target_flag.push_back("-target");
        target_flag.push_back(tc.target.triple());
    }

    auto compile = [&](std::vector<std::string> flags,
                       const std::string& dir,
                       const std::string& filename,
                       const std::string& prefix) -> Path {
        std::string stem = filename.substr(0, filename.rfind('.'));
        for (auto& c : stem) if (c == '/') c = '_';
        Path obj = cache_dir / (prefix + "__" + stem + ".o");
        if (obj.is_regular_file()) return obj;

        flags.push_back(dir + "/" + filename);
        flags.push_back("-o");
        flags.push_back(obj.string());

        auto r = run_process(flags, Path(), true);
        if (!r.success()) {
            std::print(std::cerr, "{}", r.stderr_output);
            std::println(std::cerr, "bake: failed to compile {}:{}", prefix, filename);
            return Path();
        }
        return obj;
    };

    auto archive = [&](const Path& ar_path, const std::vector<Path>& objs) {
        write_archive(ar_path, objs, tc.target.is_darwin(), tc.target.is_windows());
    };

    int total = 0;

    // ── libunwind (musl only — darwin uses libSystem) ──
    if (needs_libunwind) {
        std::vector<Path> objs;

        for (auto* f : libunwind_c_files) {
            std::vector<std::string> flags = {
                tc.exe_path, "cc",
            };
            for (auto& t : target_flag) flags.push_back(t);
            flags.insert(flags.end(), {
                "-c", "-std=c99", "-fexceptions",
                "-I" + libunwind_inc,
                "-D_LIBUNWIND_HIDE_SYMBOLS",
                "-D_LIBUNWIND_IS_NATIVE_ONLY",
                "-fvisibility=hidden", "-fPIC", "-Os", "-w",
            });
            auto obj = compile(flags, libunwind_src, f, "unwind");
            if (obj.string().empty()) return result;
            objs.push_back(obj);
            ++total;
        }
        for (auto* f : libunwind_cpp_files) {
            std::vector<std::string> flags = {
                tc.exe_path, "c++",
            };
            for (auto& t : target_flag) flags.push_back(t);
            flags.insert(flags.end(), {
                "-c", "-std=c++23", "-fno-exceptions", "-fno-rtti",
                "-I" + libunwind_inc,
                "-D_LIBUNWIND_HIDE_SYMBOLS",
                "-D_LIBUNWIND_IS_NATIVE_ONLY",
                "-fvisibility=hidden", "-fvisibility-inlines-hidden",
                "-fPIC", "-Os", "-w",
            });
            auto obj = compile(flags, libunwind_src, f, "unwind");
            if (obj.string().empty()) return result;
            objs.push_back(obj);
            ++total;
        }
        for (auto* f : libunwind_asm_files) {
            std::vector<std::string> flags = {
                tc.exe_path, "cc",
            };
            for (auto& t : target_flag) flags.push_back(t);
            flags.insert(flags.end(), {"-c", "-I" + libunwind_inc, "-w"});
            auto obj = compile(flags, libunwind_src, f, "unwind");
            if (obj.string().empty()) return result;
            objs.push_back(obj);
            ++total;
        }
        archive(result.libunwind_a, objs);
    }

    // ── libc++abi ──
    {
        std::vector<std::string> flags = {tc.exe_path, "c++"};
        for (auto& t : target_flag) flags.push_back(t);
        flags.insert(flags.end(), {
            "-c", "-std=c++23", "-DNDEBUG",
            "-I" + libcxxabi_inc,
            "-I" + libcxx_src,
            "-I" + libcxx_libc,
            "-fPIC", "-Os",
            "-D_LIBCPP_BUILDING_LIBRARY",
            "-DLIBCXX_BUILDING_LIBCXXABI",
            "-D_LIBCPP_DISABLE_VISIBILITY_ANNOTATIONS",
            "-fvisibility=hidden", "-fvisibility-inlines-hidden",
            "-faligned-allocation",
            "-D_LIBCXXABI_BUILDING_LIBRARY",
        });

        std::vector<Path> objs;
        for (auto* f : libcxxabi_files) {
            std::string fname(f);
            if (fname == "cxa_noexception.cpp") continue;
            auto obj = compile(flags, libcxxabi_src, fname, "libcxxabi");
            if (obj.string().empty()) return result;
            objs.push_back(obj);
            ++total;
        }
        archive(result.libcxxabi_a, objs);
    }

    // ── libc++ ──
    {
        std::vector<std::string> flags = {tc.exe_path, "c++"};
        for (auto& t : target_flag) flags.push_back(t);
        flags.insert(flags.end(), {
            "-c", "-std=c++23", "-DNDEBUG",
            "-I" + libcxxabi_inc,
            "-I" + libcxx_src,
            "-I" + libcxx_libc,
            "-fPIC", "-Os",
            "-D_LIBCPP_BUILDING_LIBRARY",
            "-DLIBCXX_BUILDING_LIBCXXABI",
            "-D_LIBCPP_DISABLE_VISIBILITY_ANNOTATIONS",
            "-fvisibility=hidden", "-fvisibility-inlines-hidden",
            "-faligned-allocation",
        });

        std::vector<Path> objs;
        std::set<std::string> compiled;
        for (auto* f : libcxx_base_files) {
            std::string fname(f);
            if (fname == "filesystem/int128_builtins.cpp") continue;
            auto obj = compile(flags, libcxx_src, fname, "libcxx");
            if (obj.string().empty()) return result;
            objs.push_back(obj);
            compiled.insert(fname);
            ++total;
        }
        for (auto* f : libcxx_thread_files) {
            std::string fname(f);
            if (compiled.count(fname)) continue;
            auto obj = compile(flags, libcxx_src, fname, "libcxx");
            if (obj.string().empty()) return result;
            objs.push_back(obj);
            ++total;
        }

        // Windows-specific support (locale, threads, etc.).
        if (tc.target.is_windows()) {
            static const char* win32_support_files[] = {
                "support/win32/locale_win32.cpp",
                "support/win32/support.cpp",
                "support/win32/thread_win32.cpp",
            };
            for (auto* f : win32_support_files) {
                auto obj = compile(flags, libcxx_src, f, "libcxx");
                if (obj.string().empty()) return result;
                objs.push_back(obj);
                ++total;
            }
        }

        archive(result.libcxx_a, objs);
    }

    std::println("   Compiled {} sources", total);
    write_file(sentinel, "");
    return result;
}

// ── musl (crt objects + libc.a) ──

export struct MuslObjects {
    Path crt1_o;    // static executable (crt1.c)
    Path rcrt1_o;   // static PIE (rcrt1.c)
    Path Scrt1_o;   // dynamic (Scrt1.c)
    Path libc_a;
};

static bool should_compile_musl_source(
        const Path& src_root, const Path& file,
        std::string_view arch) {
    auto rel = std::filesystem::relative(file.fs(), src_root.fs());
    if (rel.empty()) return false;
    auto generic = rel.lexically_normal().generic_string();

    std::string arch_token = "/" + std::string(arch) + "/";
    auto arch_pos = generic.find(arch_token);
    if (arch_pos != std::string::npos) return true;

    static const std::vector<std::string> all_archs = {
        "aarch64", "arm", "i386", "x86_64", "riscv64", "riscv32",
        "mips", "mips64", "mipsn32", "powerpc", "powerpc64",
        "s390x", "sh", "x32", "loongarch64", "m68k", "microblaze", "or1k"
    };
    for (const auto& a : all_archs) {
        if (a == arch) continue;
        if (generic.find("/" + a + "/") != std::string::npos)
            return false;
    }

    auto slash = generic.rfind('/');
    if (slash == std::string::npos) return true;
    std::string dir = generic.substr(0, slash);
    std::string base = generic.substr(slash + 1);
    auto dot = base.rfind('.');
    std::string stem = (dot != std::string::npos) ? base.substr(0, dot) : base;

    for (auto ext : {".c", ".s", ".S"}) {
        Path override_path = src_root / dir / std::string(arch) / (stem + ext);
        if (override_path.is_regular_file()) return false;
    }

    return true;
}

export MuslObjects ensure_musl_objects(const Toolchain& tc) {
    MuslObjects result;
    if (!tc.target.is_linux_musl()) return result;

    Path lib = find_lib_dir();
    if (lib.string().empty()) return result;

    Path musl_src = lib / "libc" / "musl";
    std::string arch = tc.target.arch();

    Path cache_root = get_cache_dir().parent() / "musl-objects";
    Path cache_dir = cache_root / tc.target.triple();

    Path sentinel = cache_dir / ".done";
    if (sentinel.is_regular_file()) {
        result.crt1_o  = cache_dir / "crt1.o";
        result.rcrt1_o = cache_dir / "rcrt1.o";
        result.Scrt1_o = cache_dir / "Scrt1.o";
        result.libc_a  = cache_dir / "libc.a";
        if (result.crt1_o.is_regular_file() && result.libc_a.is_regular_file())
            return result;
    }

    cache_dir.mkdir_recursive();
    std::println("   Compiling musl for {} (cached)", tc.target.triple());

    write_file(cache_dir / "version.h",
               "#define VERSION \"" + std::string("1.2.5") + "\"\n");

    auto make_flags = [&]() {
        std::vector<std::string> flags;
        flags.push_back(tc.exe_path);
        flags.push_back("cc");
        flags.push_back("-target");
        flags.push_back(tc.target.triple());
        flags.push_back("-c");
        flags.push_back("-std=c99");
        flags.push_back("-ffreestanding");
        flags.push_back("-fexcess-precision=standard");
        flags.push_back("-frounding-math");
        flags.push_back("-ffp-contract=off");
        flags.push_back("-fno-strict-aliasing");
        flags.push_back("-Wa,--noexecstack");
        flags.push_back("-D_XOPEN_SOURCE=700");
        flags.push_back("-Os");
        flags.push_back("-w");
        flags.push_back("-I" + (musl_src / "arch" / arch).string());
        flags.push_back("-I" + (musl_src / "arch" / "generic").string());
        flags.push_back("-I" + (musl_src / "src" / "include").string());
        flags.push_back("-I" + (musl_src / "src" / "internal").string());
        flags.push_back("-I" + cache_dir.string());
        flags.push_back("-I" + (musl_src / "include").string());
        return flags;
    };

    auto base_flags = make_flags();
    auto flags_03 = base_flags;
    std::replace(flags_03.begin(), flags_03.end(), std::string("-Os"),
                 std::string("-O3"));

    auto is_hot_path = [](const std::string& rel) {
        return rel.find("malloc/") != std::string::npos ||
               rel.find("string/") != std::string::npos ||
               rel.find("internal/") != std::string::npos;
    };

    // ── Compile crt objects (static / static-PIE / dynamic) ──
    auto compile_crt = [&](const char* src_name, const char* out_name,
                           bool need_pic) -> bool {
        Path src = musl_src / "crt" / src_name;
        Path obj = cache_dir / out_name;
        if (obj.is_regular_file()) { return true; }
        if (!src.is_regular_file()) { return true; }  // source may not exist
        auto cmd = base_flags;
        cmd.push_back("-DCRT");
        if (need_pic) cmd.push_back("-fPIC");
        cmd.push_back(src.string());
        cmd.push_back("-o");
        cmd.push_back(obj.string());
        auto r = run_process(cmd, Path(), true);
        if (!r.success()) {
            std::print(std::cerr, "{}", r.stderr_output);
            std::println(std::cerr, "bake: failed to compile {}", src_name);
            return false;
        }
        return true;
    };

    result.crt1_o = cache_dir / "crt1.o";
    if (!compile_crt("crt1.c", "crt1.o", false)) return result;

    result.rcrt1_o = cache_dir / "rcrt1.o";
    if (!compile_crt("rcrt1.c", "rcrt1.o", true)) return result;

    result.Scrt1_o = cache_dir / "Scrt1.o";
    if (!compile_crt("Scrt1.c", "Scrt1.o", true)) return result;

    // ── Compile libc.a ──
    result.libc_a = cache_dir / "libc.a";

    namespace fs = std::filesystem;
    std::vector<Path> sources;
    for (auto& entry : fs::recursive_directory_iterator((musl_src / "src").fs())) {
        auto ext = entry.path().extension().string();
        if (ext == ".c" || ext == ".s" || ext == ".S") {
            Path f(entry.path());
            if (should_compile_musl_source(musl_src / "src", f, arch))
                sources.push_back(std::move(f));
        }
    }
    std::sort(sources.begin(), sources.end());

    std::vector<Path> obj_files;
    int compiled = 0;
    for (const auto& src : sources) {
        auto rel = fs::relative(src.fs(), (musl_src / "src").fs());
        std::string stem = rel.lexically_normal().generic_string();
        for (auto& c : stem) if (c == '/') c = '_';
        stem = stem.substr(0, stem.rfind('.'));
        Path obj = cache_dir / (stem + ".o");

        if (!obj.is_regular_file()) {
            auto rel_str = rel.lexically_normal().generic_string();
            auto& flags = is_hot_path(rel_str) ? flags_03 : base_flags;
            auto cmd = flags;
            cmd.push_back(src.string());
            cmd.push_back("-o");
            cmd.push_back(obj.string());
            auto r = run_process(cmd, Path(), true);
            if (!r.success()) {
                std::print(std::cerr, "{}", r.stderr_output);
                std::println(std::cerr, "bake: failed to compile {}", rel_str);
                return result;
            }
            ++compiled;
        }
        obj_files.push_back(obj);
    }

    if (!obj_files.empty()) {
        if (!write_archive(result.libc_a, obj_files, false)) {
            std::println(std::cerr, "bake: failed to create libc.a");
            return result;
        }
    }

    std::println("   Compiled {} musl sources", compiled);
    write_file(sentinel, "");

    return result;
}

// ── compiler-rt builtins ──
//
// Built for ALL targets (not just musl), matching the upstream design.
// For darwin, a short exclude list (from Darwin-excludes/osx.txt) skips
// builtins already provided by libSystem (128-bit float ops, trampoline).

// Symbols already provided by libSystem on macOS — must not be in our archive.
static const std::unordered_set<std::string> darwin_excludes = {
    "apple_versioning", "addtf3", "divtf3", "multf3",
    "powitf2", "subtf3", "trampoline_setup",
};

export Path ensure_compiler_rt_objects(const Toolchain& tc) {
    Path lib = find_lib_dir();
    if (lib.string().empty()) return Path();

    Path builtins_dir = lib / "compiler-rt" / "lib" / "builtins";
    Path cache_dir = get_cache_dir().parent() / "compiler-rt" / tc.target.triple();
    Path result_a = cache_dir / "libcompiler_rt.a";
    Path sentinel = cache_dir / ".compiler-rt-done";

    if (sentinel.is_regular_file() && result_a.is_regular_file())
        return result_a;

    bool is_darwin = tc.target.is_darwin();
    std::string arch = tc.target.arch();

    cache_dir.mkdir_recursive();

    auto make_flags = [&]() {
        std::vector<std::string> flags;
        flags.push_back(tc.exe_path);
        flags.push_back("cc");
        if (!tc.target.is_native()) {
            flags.push_back("-target");
            flags.push_back(tc.target.triple());
        }
        flags.push_back("-c");
        flags.push_back("-ffreestanding");
        flags.push_back("-Os");
        flags.push_back("-w");
        flags.push_back("-fPIC");
        flags.push_back("-I" + builtins_dir.string());
        return flags;
    };
    auto base_flags = make_flags();

    namespace fs = std::filesystem;
    std::vector<Path> obj_files;

    for (auto& entry : fs::directory_iterator(builtins_dir.fs())) {
        if (entry.path().extension() != ".c") continue;
        Path src(entry.path());
        std::string stem = src.stem().string();
        if (is_darwin && darwin_excludes.count(stem)) continue;
        Path obj = cache_dir / ("crt__" + src.filename().string() + ".o");
        if (!obj.is_regular_file()) {
            auto cmd = base_flags;
            cmd.push_back(src.string());
            cmd.push_back("-o");
            cmd.push_back(obj.string());
            auto r = run_process(cmd, Path(), true);
            if (!r.success()) continue;
        }
        obj_files.push_back(obj);
    }

    Path arch_dir = builtins_dir / arch;
    if (arch_dir.is_directory()) {
        for (auto& entry : fs::recursive_directory_iterator(arch_dir.fs())) {
            auto ext = entry.path().extension().string();
            if (ext != ".c" && ext != ".cpp" && ext != ".S" && ext != ".s")
                continue;
            Path src(entry.path());
            auto rel = fs::relative(src.fs(), arch_dir.fs());
            std::string stem = rel.lexically_normal().generic_string();
            for (auto& c : stem) if (c == '/') c = '_';
            stem = stem.substr(0, stem.rfind('.'));
            Path obj = cache_dir / ("crt__" + arch + "__" + stem + ".o");
            if (!obj.is_regular_file()) {
                auto cmd = base_flags;
                cmd.push_back(src.string());
                cmd.push_back("-o");
                cmd.push_back(obj.string());
                auto r = run_process(cmd, Path(), true);
                if (!r.success()) continue;
            }
            obj_files.push_back(obj);
        }
    }

    if (!obj_files.empty()) {
        write_archive(result_a, obj_files, is_darwin, tc.target.is_windows());
    }

    write_file(sentinel, "");
    return result_a;
}

// ── mingw-w64 (windows-gnu) ──
//
// Builds CRT objects + libmingw32.a from vendored MinGW-w64 source, and
// generates import libraries from .def files using LLD's COFF driver.
// Cache is global, keyed by target triple.

export struct MingwObjects {
    Path crt2_o;          // exe entry point (from crt/crtexe.c)
    Path dllcrt2_o;       // DLL entry point (from crt/crtdll.c)
    Path libmingw32_a;    // static helper library (includes winpthreads)
    Path import_lib_dir;  // generated import libraries (.lib from .def)
};

// Source file lists — curated from MinGW-w64, matching Zig's selection.
// See https://github.com/ziglang/zig/blob/master/src/libs/mingw.zig

static const char* mingw32_crt_src[] = {
    "crt/crtexewin.c", "crt/dll_argv.c", "crt/gccmain.c",
    "crt/natstart.c", "crt/pseudo-reloc-list.c", "crt/wildcard.c",
    "crt/charmax.c", "crt/ucrtexewin.c", "crt/dllargv.c",
    "crt/_newmode.c", "crt/tlssup.c", "crt/xncommod.c",
    "crt/cinitexe.c", "crt/merr.c", "crt/usermatherr.c",
    "crt/pesect.c", "crt/udllargc.c", "crt/xthdloc.c",
    "crt/mingw_helpers.c", "crt/pseudo-reloc.c", "crt/udll_argv.c",
    "crt/xtxtmode.c", "crt/crt_handler.c", "crt/tlsthrd.c",
    "crt/tlsmthread.c", "crt/tlsmcrt.c", "crt/cxa_atexit.c",
    "crt/cxa_thread_atexit.c", "crt/tls_atexit.c",
    "intrincs/RtlSecureZeroMemory.c",
};

static const char* mingw32_complex_src[] = {
    "complex/_cabs.c", "complex/cabs.c", "complex/cabsf.c",
    "complex/cabsl.c", "complex/cacos.c", "complex/cacosf.c",
    "complex/cacosl.c", "complex/carg.c", "complex/cargf.c",
    "complex/cargl.c", "complex/casin.c", "complex/casinf.c",
    "complex/casinl.c", "complex/catan.c", "complex/catanf.c",
    "complex/catanl.c", "complex/ccos.c", "complex/ccosf.c",
    "complex/ccosl.c", "complex/cexp.c", "complex/cexpf.c",
    "complex/cexpl.c", "complex/cimag.c", "complex/cimagf.c",
    "complex/cimagl.c", "complex/clog.c", "complex/clog10.c",
    "complex/clog10f.c", "complex/clog10l.c", "complex/clogf.c",
    "complex/clogl.c", "complex/conj.c", "complex/conjf.c",
    "complex/conjl.c", "complex/cpow.c", "complex/cpowf.c",
    "complex/cpowl.c", "complex/cproj.c", "complex/cprojf.c",
    "complex/cprojl.c", "complex/creal.c", "complex/crealf.c",
    "complex/creall.c", "complex/csin.c", "complex/csinf.c",
    "complex/csinl.c", "complex/csqrt.c", "complex/csqrtf.c",
    "complex/csqrtl.c", "complex/ctan.c", "complex/ctanf.c",
    "complex/ctanl.c",
};

static const char* mingw32_gdtoa_src[] = {
    "gdtoa/arithchk.c", "gdtoa/dmisc.c", "gdtoa/dtoa.c",
    "gdtoa/g__fmt.c", "gdtoa/g_dfmt.c", "gdtoa/g_ffmt.c",
    "gdtoa/g_xfmt.c", "gdtoa/gdtoa.c", "gdtoa/gethex.c",
    "gdtoa/gmisc.c", "gdtoa/hd_init.c", "gdtoa/hexnan.c",
    "gdtoa/misc.c", "gdtoa/qnan.c", "gdtoa/smisc.c",
    "gdtoa/strtodg.c", "gdtoa/strtodnrp.c", "gdtoa/strtof.c",
    "gdtoa/strtopx.c", "gdtoa/sum.c", "gdtoa/ulp.c",
};

static const char* mingw32_math_src[] = {
    "math/coshl.c", "math/fp_consts.c", "math/fp_constsf.c",
    "math/fp_constsl.c", "math/fpclassify.c", "math/fpclassifyf.c",
    "math/fpclassifyl.c", "math/frexpf.c", "math/frexpl.c",
    "math/hypotf.c", "math/hypotl.c", "math/ldexpf.c",
    "math/lgamma.c", "math/lgammaf.c", "math/lgammal.c",
    "math/modfl.c", "math/powi.c", "math/powif.c",
    "math/powil.c", "math/signbit.c", "math/signbitf.c",
    "math/signbitl.c", "math/signgam.c", "math/sinhl.c",
    "math/sqrtl.c", "math/tanhl.c",
    // ucrtbase
    "math/_huge.c",
};

static const char* mingw32_misc_src[] = {
    "misc/alarm.c", "misc/btowc.c", "misc/delay-f.c",
    "misc/delay-n.c", "misc/delayimp.c", "misc/dirent.c",
    "misc/dirname.c", "misc/dllmain.c", "misc/feclearexcept.c",
    "misc/fegetenv.c", "misc/fegetexceptflag.c", "misc/fegetround.c",
    "misc/feholdexcept.c", "misc/feraiseexcept.c", "misc/fesetenv.c",
    "misc/fesetexceptflag.c", "misc/fesetround.c", "misc/fetestexcept.c",
    "misc/mingw_controlfp.c", "misc/mingw_setfp.c", "misc/feupdateenv.c",
    "misc/ftruncate.c", "misc/ftw32.c", "misc/ftw32i64.c",
    "misc/ftw64.c", "misc/ftw64i32.c", "misc/fwide.c",
    "misc/getlogin.c", "misc/getopt.c", "misc/gettimeofday.c",
    "misc/mempcpy.c", "misc/mingw-access.c", "misc/mingw-aligned-malloc.c",
    "misc/mingw_getsp.S", "misc/mingw_longjmp.S", "misc/mingw_matherr.c",
    "misc/mingw_mbwc_convert.c", "misc/mingw_usleep.c",
    "misc/mingw_wcstod.c", "misc/mingw_wcstof.c", "misc/mingw_wcstold.c",
    "misc/mkstemp.c", "misc/sleep.c", "misc/strnlen.c",
    "misc/strsafe.c", "misc/tdelete.c", "misc/tdestroy.c",
    "misc/tfind.c", "misc/tsearch.c", "misc/twalk.c",
    "misc/wcsnlen.c", "misc/wcstof.c", "misc/wcstoimax.c",
    "misc/wcstold.c", "misc/wcstoumax.c", "misc/wctob.c",
    "misc/wdirent.c", "misc/winbs_uint64.c", "misc/winbs_ulong.c",
    "misc/winbs_ushort.c", "misc/wmemchr.c", "misc/wmemcmp.c",
    "misc/wmemcpy.c", "misc/wmemmove.c", "misc/wmempcpy.c",
    "misc/wmemset.c",
    // ucrtbase
    "misc/__initenv.c", "misc/__winitenv.c", "misc/__p___initenv.c",
    "misc/__p___winitenv.c", "misc/_onexit.c", "misc/ucrt-access.c",
    "misc/ucrt__getmainargs.c", "misc/ucrt__wgetmainargs.c",
    "misc/ucrt_amsg_exit.c", "misc/ucrt_at_quick_exit.c",
    "misc/ucrt_tzset.c",
};

static const char* mingw32_stdio_src[] = {
    "stdio/_Exit.c", "stdio/_findfirst64i32.c", "stdio/_findnext64i32.c",
    "stdio/_fstat64i32.c", "stdio/_stat64i32.c", "stdio/_wfindfirst64i32.c",
    "stdio/_wfindnext64i32.c", "stdio/_wstat64i32.c",
    "stdio/__mingw_fix_stat_path.c", "stdio/__mingw_fix_wstat_path.c",
    "stdio/asprintf.c", "stdio/fopen64.c", "stdio/fseeko32.c",
    "stdio/fseeko64.c", "stdio/ftello.c", "stdio/ftello64.c",
    "stdio/ftruncate64.c", "stdio/lltoa.c", "stdio/lltow.c",
    "stdio/lseek64.c", "stdio/mingw_asprintf.c", "stdio/mingw_fprintf.c",
    "stdio/mingw_fwprintf.c", "stdio/mingw_fscanf.c",
    "stdio/mingw_fwscanf.c", "stdio/mingw_pformat.c",
    "stdio/mingw_sformat.c", "stdio/mingw_swformat.c",
    "stdio/mingw_wpformat.c", "stdio/mingw_printf.c",
    "stdio/mingw_wprintf.c", "stdio/mingw_scanf.c",
    "stdio/mingw_snprintf.c", "stdio/mingw_snwprintf.c",
    "stdio/mingw_sprintf.c", "stdio/mingw_swprintf.c",
    "stdio/mingw_sscanf.c", "stdio/mingw_swscanf.c",
    "stdio/mingw_vasprintf.c", "stdio/mingw_vfprintf.c",
    "stdio/mingw_vfwprintf.c", "stdio/mingw_vfscanf.c",
    "stdio/mingw_vprintf.c", "stdio/mingw_vsscanf.c",
    "stdio/mingw_vwprintf.c", "stdio/mingw_vsnprintf.c",
    "stdio/mingw_vsnwprintf.c", "stdio/mingw_vsprintf.c",
    "stdio/mingw_vswprintf.c", "stdio/mingw_wscanf.c",
    "stdio/mingw_vfwscanf.c", "stdio/mingw_vswscanf.c",
    "stdio/snprintf.c", "stdio/snwprintf.c", "stdio/strtok_r.c",
    "stdio/truncate.c", "stdio/ulltoa.c", "stdio/ulltow.c",
    "stdio/vasprintf.c", "stdio/vsnprintf.c", "stdio/vsnwprintf.c",
    "stdio/wtoll.c",
    // ucrtbase
    "stdio/ucrt__scprintf.c", "stdio/ucrt__snprintf.c",
    "stdio/ucrt__snscanf.c", "stdio/ucrt__snwprintf.c",
    "stdio/ucrt__vscprintf.c", "stdio/ucrt__vsnprintf.c",
    "stdio/ucrt__vsnwprintf.c", "stdio/ucrt___local_stdio_printf_options.c",
    "stdio/ucrt___local_stdio_scanf_options.c", "stdio/ucrt_fprintf.c",
    "stdio/ucrt_fscanf.c", "stdio/ucrt_fwprintf.c",
    "stdio/ucrt_fwscanf.c", "stdio/ucrt_ms_fwprintf.c",
    "stdio/ucrt_printf.c", "stdio/ucrt_scanf.c",
    "stdio/ucrt_snprintf.c", "stdio/ucrt_snwprintf.c",
    "stdio/ucrt_sprintf.c", "stdio/ucrt_sscanf.c",
    "stdio/ucrt_swscanf.c", "stdio/ucrt_swprintf.c",
    "stdio/ucrt_vfprintf.c", "stdio/ucrt_vfscanf.c",
    "stdio/ucrt_vfwscanf.c", "stdio/ucrt_vfwprintf.c",
    "stdio/ucrt_vprintf.c", "stdio/ucrt_vscanf.c",
    "stdio/ucrt_vsnprintf.c", "stdio/ucrt_vsnwprintf.c",
    "stdio/ucrt_vsprintf.c", "stdio/ucrt_vswprintf.c",
    "stdio/ucrt_vsscanf.c", "stdio/ucrt_vwscanf.c",
    "stdio/ucrt_wscanf.c", "stdio/ucrt_vwprintf.c",
    "stdio/ucrt_wprintf.c",
};

static const char* mingw32_string_src[] = {
    "string/ucrt__wcstok.c",
};

static const char* mingw32_libsrc_src[] = {
    "libsrc/ativscp-uuid.c", "libsrc/atsmedia-uuid.c",
    "libsrc/bth-uuid.c", "libsrc/cguid-uuid.c",
    "libsrc/comcat-uuid.c", "libsrc/ctxtcall-uuid.c",
    "libsrc/devguid.c", "libsrc/docobj-uuid.c",
    "libsrc/dxva-uuid.c", "libsrc/exdisp-uuid.c",
    "libsrc/extras-uuid.c", "libsrc/fwp-uuid.c",
    "libsrc/guid_nul.c", "libsrc/hlguids-uuid.c",
    "libsrc/hlink-uuid.c", "libsrc/mlang-uuid.c",
    "libsrc/msctf-uuid.c", "libsrc/mshtmhst-uuid.c",
    "libsrc/mshtml-uuid.c", "libsrc/msxml-uuid.c",
    "libsrc/netcfg-uuid.c", "libsrc/netcon-uuid.c",
    "libsrc/ntddkbd-uuid.c", "libsrc/ntddmou-uuid.c",
    "libsrc/ntddpar-uuid.c", "libsrc/ntddscsi-uuid.c",
    "libsrc/ntddser-uuid.c", "libsrc/ntddstor-uuid.c",
    "libsrc/ntddvdeo-uuid.c", "libsrc/oaidl-uuid.c",
    "libsrc/objidl-uuid.c", "libsrc/objsafe-uuid.c",
    "libsrc/ocidl-uuid.c", "libsrc/oleacc-uuid.c",
    "libsrc/olectlid-uuid.c", "libsrc/oleidl-uuid.c",
    "libsrc/power-uuid.c", "libsrc/powrprof-uuid.c",
    "libsrc/uianimation-uuid.c", "libsrc/usbcamdi-uuid.c",
    "libsrc/usbiodef-uuid.c", "libsrc/uuid.c",
    "libsrc/vds-uuid.c", "libsrc/virtdisk-uuid.c",
    "libsrc/vss-uuid.c", "libsrc/wia-uuid.c",
    "libsrc/windowscodecs.c",
    "libsrc/ws2_32.c",
    "libsrc/ws2tcpip/in6_addr_equal.c", "libsrc/ws2tcpip/in6addr_isany.c",
    "libsrc/ws2tcpip/in6addr_isloopback.c", "libsrc/ws2tcpip/in6addr_setany.c",
    "libsrc/ws2tcpip/in6addr_setloopback.c", "libsrc/ws2tcpip/in6_is_addr_linklocal.c",
    "libsrc/ws2tcpip/in6_is_addr_loopback.c", "libsrc/ws2tcpip/in6_is_addr_mc_global.c",
    "libsrc/ws2tcpip/in6_is_addr_mc_linklocal.c", "libsrc/ws2tcpip/in6_is_addr_mc_nodelocal.c",
    "libsrc/ws2tcpip/in6_is_addr_mc_orglocal.c", "libsrc/ws2tcpip/in6_is_addr_mc_sitelocal.c",
    "libsrc/ws2tcpip/in6_is_addr_multicast.c", "libsrc/ws2tcpip/in6_is_addr_sitelocal.c",
    "libsrc/ws2tcpip/in6_is_addr_unspecified.c", "libsrc/ws2tcpip/in6_is_addr_v4compat.c",
    "libsrc/ws2tcpip/in6_is_addr_v4mapped.c", "libsrc/ws2tcpip/in6_set_addr_loopback.c",
    "libsrc/ws2tcpip/in6_set_addr_unspecified.c", "libsrc/ws2tcpip/gai_strerrorA.c",
    "libsrc/ws2tcpip/gai_strerrorW.c",
    "libsrc/wspiapi/WspiapiStrdup.c", "libsrc/wspiapi/WspiapiParseV4Address.c",
    "libsrc/wspiapi/WspiapiNewAddrInfo.c", "libsrc/wspiapi/WspiapiQueryDNS.c",
    "libsrc/wspiapi/WspiapiLookupNode.c", "libsrc/wspiapi/WspiapiClone.c",
    "libsrc/wspiapi/WspiapiLegacyFreeAddrInfo.c", "libsrc/wspiapi/WspiapiLegacyGetAddrInfo.c",
    "libsrc/wspiapi/WspiapiLegacyGetNameInfo.c", "libsrc/wspiapi/WspiapiLoad.c",
    "libsrc/wspiapi/WspiapiGetAddrInfo.c", "libsrc/wspiapi/WspiapiGetNameInfo.c",
    "libsrc/wspiapi/WspiapiFreeAddrInfo.c",
    "libsrc/dinput_kbd.c", "libsrc/dinput_joy.c", "libsrc/dinput_joy2.c",
    "libsrc/dinput_mouse.c", "libsrc/dinput_mouse2.c",
    "libsrc/dloadhelper.c",
    "libsrc/bits.c", "libsrc/shell32.c", "libsrc/dmoguids.c",
    "libsrc/dxerr8.c", "libsrc/dxerr8w.c", "libsrc/dxerr9.c",
    "libsrc/dxerr9w.c", "libsrc/mfuuid.c", "libsrc/msxml2.c",
    "libsrc/msxml6.c", "libsrc/amstrmid.c", "libsrc/wbemuuid.c",
    "libsrc/wmcodecdspuuid.c", "libsrc/dxguid.c", "libsrc/ksuser.c",
    "libsrc/largeint.c", "libsrc/locationapi.c", "libsrc/sapi.c",
    "libsrc/sensorsapi.c", "libsrc/portabledeviceguids.c",
    "libsrc/taskschd.c", "libsrc/strmiids.c", "libsrc/gdiplus.c",
    "libsrc/activeds-uuid.c",
};

static const char* mingw32_other_src[] = {
    "cfguard/mingw_cfguard_support.c",
    "libsrc/mingwthrd_mt.c",
};

static const char* mingw32_winpthreads_src[] = {
    "winpthreads/barrier.c", "winpthreads/clock.c", "winpthreads/cond.c",
    "winpthreads/misc.c", "winpthreads/mutex.c", "winpthreads/nanosleep.c",
    "winpthreads/rwlock.c", "winpthreads/sched.c", "winpthreads/sem.c",
    "winpthreads/spinlock.c", "winpthreads/thread.c",
};

// x86-family (x86 + x86_64) math sources.
static const char* mingw32_x86_src[] = {
    "math/x86/_chgsignl.S",
    "math/x86/acoshl.c", "math/x86/acosl.c", "math/x86/asinhl.c",
    "math/x86/asinl.c", "math/x86/atan2l.c", "math/x86/atanhl.c",
    "math/x86/atanl.c", "math/x86/copysignl.S", "math/x86/cosl.c",
    "math/x86/cosl_internal.S", "math/x86/cossinl.c",
    "math/x86/exp2l.S", "math/x86/expl.c", "math/x86/expm1l.c",
    "math/x86/fmodl.c", "math/x86/fucom.c", "math/x86/ilogbl.S",
    "math/x86/internal_logl.S", "math/x86/ldexp.c", "math/x86/ldexpl.c",
    "math/x86/log10l.S", "math/x86/log1pl.S", "math/x86/log2l.S",
    "math/x86/logbl.c", "math/x86/logl.c", "math/x86/nearbyintl.S",
    "math/x86/powl.c", "math/x86/remainderl.S", "math/x86/remquol.S",
    "math/x86/scalbn.S", "math/x86/scalbnf.S", "math/x86/scalbnl.S",
    "math/x86/sinl.c", "math/x86/sinl_internal.S", "math/x86/tanl.S",
    "math/cbrtl.c", "math/erfl.c", "math/fdiml.c", "math/fmal.c",
    "math/fmaxl.c", "math/fminl.c", "math/llrintl.c", "math/llroundl.c",
    "math/lrintl.c", "math/lroundl.c", "math/rintl.c", "math/roundl.c",
    "math/tgammal.c", "math/nextafterl.c", "math/nexttoward.c",
    "math/nexttowardf.c",
    "crt/CRT_fp10.c",
};

// Libraries that must always be linked on Windows targets.
static const char* mingw_always_link_libs[] = {
    "api-ms-win-crt-conio-l1-1-0", "api-ms-win-crt-convert-l1-1-0",
    "api-ms-win-crt-environment-l1-1-0", "api-ms-win-crt-filesystem-l1-1-0",
    "api-ms-win-crt-heap-l1-1-0", "api-ms-win-crt-locale-l1-1-0",
    "api-ms-win-crt-math-l1-1-0", "api-ms-win-crt-multibyte-l1-1-0",
    "api-ms-win-crt-private-l1-1-0", "api-ms-win-crt-process-l1-1-0",
    "api-ms-win-crt-runtime-l1-1-0", "api-ms-win-crt-stdio-l1-1-0",
    "api-ms-win-crt-string-l1-1-0", "api-ms-win-crt-time-l1-1-0",
    "api-ms-win-crt-utility-l1-1-0",
    "advapi32", "kernel32", "ntdll", "shell32", "user32",
};

export MingwObjects ensure_mingw_objects(const Toolchain& tc) {
    MingwObjects result;
    if (!tc.target.is_windows_gnu()) return result;

    Path lib = find_lib_dir();
    if (lib.string().empty()) return result;

    Path mingw_src = lib / "libc" / "mingw";
    if (!mingw_src.is_directory()) {
        std::println(std::cerr,
            "bake: MinGW-w64 source not found at {}\n"
            "  Run scripts/fetch-mingw.sh to vendor MinGW-w64 for windows-gnu targets.",
            mingw_src.string());
        return result;
    }

    Path cache_dir = get_cache_dir().parent() / "mingw-objects" / tc.target.triple();
    Path sentinel = cache_dir / ".done";

    result.import_lib_dir = cache_dir / "implib";

    if (sentinel.is_regular_file()) {
        result.crt2_o       = cache_dir / "crt2.o";
        result.dllcrt2_o    = cache_dir / "dllcrt2.o";
        result.libmingw32_a = cache_dir / "libmingw32.a";
        if (result.crt2_o.is_regular_file() &&
            result.libmingw32_a.is_regular_file())
            return result;
    }

    cache_dir.mkdir_recursive();
    result.import_lib_dir.mkdir_recursive();
    std::println("   Compiling mingw-w64 for {} (cached)", tc.target.triple());

    std::string arch = tc.target.arch();
    std::string machine = (arch == "x86_64") ? "X64" : "ARM64";
    std::string def_arch_dir = (arch == "x86_64") ? "lib64" : "libarm64";

    // ── Common compile flags ──
    auto make_base_flags = [&]() {
        std::vector<std::string> flags;
        flags.push_back(tc.exe_path);
        flags.push_back("cc");
        flags.push_back("-target");
        flags.push_back(tc.target.triple());
        flags.push_back("-c");
        flags.push_back("-std=gnu11");
        flags.push_back("-D__USE_MINGW_ANSI_STDIO=0");
        flags.push_back("-isystem");
        flags.push_back((lib / "libc" / "include" / "any-windows-any").string());
        flags.push_back("-Os");
        flags.push_back("-w");
        return flags;
    };

    // CRT-specific extra flags (for crt2.o, dllcrt2.o, and libmingw32.a sources).
    auto crt_extra = std::vector<std::string>{
        "-D__MSVCRT_VERSION__=0x700",
        "-D_CTYPE_DISABLE_MACROS",  // Call CRT functions directly, not via iswctype macros
        "-D_CRTBLD",
        "-D_SYSCRT=1",
        "-D_WIN32_WINNT=0x0f00",
        "-DCRTDLL=1",
        "-I" + (mingw_src / "include").string(),
    };

    auto compile = [&](std::vector<std::string> flags,
                       const std::string& filename,
                       const std::string& prefix) -> Path {
        std::string stem = filename;
        for (auto& c : stem) if (c == '/' || c == '.') c = '_';
        Path obj = cache_dir / (prefix + "__" + stem + ".o");
        if (obj.is_regular_file()) return obj;

        flags.push_back((mingw_src / filename).string());
        flags.push_back("-o");
        flags.push_back(obj.string());

        auto r = run_process(flags, Path(), true);
        if (!r.success()) {
            std::print(std::cerr, "{}", r.stderr_output);
            std::println(std::cerr, "bake: failed to compile {}:{}", prefix, filename);
            return Path();
        }
        return obj;
    };

    int total = 0;

    // ── crt2.o (exe entry point) ──
    {
        result.crt2_o = cache_dir / "crt2.o";
        if (!result.crt2_o.is_regular_file()) {
            auto flags = make_base_flags();
            for (auto& f : crt_extra) flags.push_back(f);
            flags.push_back((mingw_src / "crt" / "crtexe.c").string());
            flags.push_back("-o");
            flags.push_back(result.crt2_o.string());
            auto r = run_process(flags, Path(), true);
            if (!r.success()) {
                std::print(std::cerr, "{}", r.stderr_output);
                std::println(std::cerr, "bake: failed to compile crt2.o");
                return result;
            }
            ++total;
        }
    }

    // ── dllcrt2.o (DLL entry point) ──
    {
        result.dllcrt2_o = cache_dir / "dllcrt2.o";
        if (!result.dllcrt2_o.is_regular_file()) {
            auto flags = make_base_flags();
            for (auto& f : crt_extra) flags.push_back(f);
            flags.push_back((mingw_src / "crt" / "crtdll.c").string());
            flags.push_back("-o");
            flags.push_back(result.dllcrt2_o.string());
            auto r = run_process(flags, Path(), true);
            if (!r.success()) {
                std::print(std::cerr, "{}", r.stderr_output);
                std::println(std::cerr, "bake: failed to compile dllcrt2.o");
                return result;
            }
            ++total;
        }
    }

    // ── libmingw32.a ──
    result.libmingw32_a = cache_dir / "libmingw32.a";
    {
        auto base_flags = make_base_flags();
        for (auto& f : crt_extra) base_flags.push_back(f);

        // winpthreads sources get -DIN_WINPTHREAD.
        auto winpthreads_flags = make_base_flags();
        winpthreads_flags.push_back("-DIN_WINPTHREAD");
        winpthreads_flags.push_back("-Wno-unknown-warning-option");
        for (auto& f : crt_extra) winpthreads_flags.push_back(f);
        winpthreads_flags.push_back(
            "-I" + (mingw_src / "winpthreads").string());

        std::vector<Path> objs;

        auto compile_list = [&](const char* const* list, std::size_t count,
                                const std::string& prefix,
                                const std::vector<std::string>& flags) -> bool {
            for (std::size_t i = 0; i < count; ++i) {
                // Skip sources that don't exist (some are arch-specific).
                Path src = mingw_src / list[i];
                if (!src.is_regular_file()) continue;
                auto obj = compile(flags, list[i], prefix);
                if (obj.string().empty()) return false;
                objs.push_back(obj);
                ++total;
            }
            return true;
        };

#define COMPILE_LIST(lst, pfx, flags) \
    compile_list(lst, sizeof(lst)/sizeof(lst[0]), pfx, flags)

        if (!COMPILE_LIST(mingw32_crt_src,       "crt",       base_flags))      return result;
        if (!COMPILE_LIST(mingw32_complex_src,    "complex",   base_flags))      return result;
        if (!COMPILE_LIST(mingw32_gdtoa_src,      "gdtoa",     base_flags))      return result;
        if (!COMPILE_LIST(mingw32_math_src,       "math",      base_flags))      return result;
        if (!COMPILE_LIST(mingw32_misc_src,       "misc",      base_flags))      return result;
        if (!COMPILE_LIST(mingw32_stdio_src,      "stdio",     base_flags))      return result;
        if (!COMPILE_LIST(mingw32_string_src,     "string",    base_flags))      return result;
        if (!COMPILE_LIST(mingw32_libsrc_src,     "libsrc",    base_flags))      return result;
        if (!COMPILE_LIST(mingw32_other_src,      "other",     base_flags))      return result;
        if (!COMPILE_LIST(mingw32_winpthreads_src,"winpthr",   winpthreads_flags)) return result;

        // x86-family sources (x86_64 only for now).
        if (arch == "x86_64" || arch == "i686" || arch == "i386") {
            if (!COMPILE_LIST(mingw32_x86_src,    "x86",       base_flags))      return result;
        }

#undef COMPILE_LIST

        write_archive(result.libmingw32_a, objs, false, true);  // COFF format
    }

    // ── Import libraries from .def files (via LLD COFF driver) ──
    {
        for (auto* lib_name : mingw_always_link_libs) {
            // LLD's COFF driver forces a "lib" prefix and ".lib" extension,
            // producing "lib<name>.lib". The MinGW driver (ld.lld) searches
            // for "<name>.lib" (pattern 4), so we rename after generation.
            Path final_lib = result.import_lib_dir /
                             (std::string(lib_name) + ".lib");
            if (final_lib.is_regular_file()) continue;

            // Find .def: arch-specific first, then generic.
            Path def_file = mingw_src / def_arch_dir /
                            (std::string(lib_name) + ".def");
            if (!def_file.is_regular_file()) {
                def_file = mingw_src / "lib-common" /
                           (std::string(lib_name) + ".def");
            }
            if (!def_file.is_regular_file()) continue;

            std::vector<const char*> lld_args;
            lld_args.push_back("lld-link");
            std::string def_arg = "/def:" + def_file.string();
            std::string out_arg = "/out:" + final_lib.string();
            std::string mach_arg = "/machine:" + machine;
            lld_args.push_back(def_arg.c_str());
            lld_args.push_back(out_arg.c_str());
            lld_args.push_back(mach_arg.c_str());

            if (bake_lld_link(LldFlavor::COFF,
                              static_cast<int>(lld_args.size()),
                              lld_args.data()) != 0) {
                std::println(std::cerr,
                    "bake: failed to generate import library for {}", lib_name);
                // Non-fatal: the linker will report unresolved symbols if needed.
            }

            // LLD creates "lib<name>.lib" — rename to "<name>.lib".
            Path lld_output = result.import_lib_dir /
                              ("lib" + std::string(lib_name) + ".lib");
            if (lld_output.is_regular_file() && !final_lib.is_regular_file()) {
                std::error_code ec;
                std::filesystem::rename(lld_output.string(),
                                        final_lib.string(), ec);
            }
            ++total;
        }
    }

    std::println("   Compiled {} mingw-w64 artifacts", total);
    write_file(sentinel, "");
    return result;
}

// ── Unified runtime preparation ──
//
// Called once per link job from bake_clang_driver.cpp. Dispatches to the
// family-specific ensure_* functions and assembles a RuntimeArtifacts
// struct. This is the ONLY function the driver needs to call — all
// target-specific decisions are encapsulated here.

export RuntimeArtifacts prepare_runtime(
        const Toolchain& tc, bool is_cxx, LinkMode link_mode) {
    RuntimeArtifacts rt;
    LibcFamily family = resolve_libc_family(tc.target);

    // compiler-rt: all targets
    rt.compiler_rt = ensure_compiler_rt_objects(tc);

    // C++ runtime: all targets when is_cxx
    if (is_cxx) {
        auto cxx = ensure_cxx_runtime(tc);
        rt.libcxx    = cxx.libcxx_a;
        rt.libcxxabi = cxx.libcxxabi_a;
        rt.libunwind = cxx.libunwind_a;
    }

    switch (family) {
    case LibcFamily::Musl: {
        auto musl = ensure_musl_objects(tc);
        rt.libc = musl.libc_a;
        switch (link_mode) {
        case LinkMode::Static:     rt.crt_entry = musl.crt1_o;  break;
        case LinkMode::StaticPie:  rt.crt_entry = musl.rcrt1_o; break;
        case LinkMode::Dynamic:    rt.crt_entry = musl.Scrt1_o; break;
        }
        break;
    }
    case LibcFamily::Darwin: {
        auto sdk_layout = resolve_darwin_sdk(tc.target);
        Path lib = find_lib_dir();

        // macOS deployment target from vendored SDKSettings.json
        Path settings = lib / "libc" / "darwin" / "SDKSettings.json";
        rt.macos_deployment_target = "15.0";
        if (auto content = read_file(settings)) {
            auto key = std::string("\"MinimalDisplayName\":\"");
            auto pos = content->find(key);
            if (pos != std::string::npos) {
                auto start = pos + key.size();
                auto end = content->find('"', start);
                if (end != std::string::npos)
                    rt.macos_deployment_target = content->substr(start, end - start);
            }
        }

        if (sdk_layout == DarwinSdkLayout::SystemSdk) {
            std::vector<std::string> xcrun_cmd = {
                "xcrun", "--sdk", "macosx", "--show-sdk-path"
            };
            auto r = run_process(xcrun_cmd, Path(), true);
            if (r.success()) {
                std::string sdk_path = r.stdout_output;
                while (!sdk_path.empty() &&
                       (sdk_path.back() == '\n' || sdk_path.back() == '\r'))
                    sdk_path.pop_back();
                if (!sdk_path.empty()) {
                    rt.link_dirs.push_back(sdk_path + "/usr/lib");
                    rt.framework_dirs.push_back(
                        sdk_path + "/System/Library/Frameworks");
                } else {
                    rt.link_dirs.push_back((lib / "libc" / "darwin").string());
                }
            } else {
                rt.link_dirs.push_back((lib / "libc" / "darwin").string());
            }
        } else {
            rt.link_dirs.push_back((lib / "libc" / "darwin").string());
        }
        break;
    }
    case LibcFamily::Windows: {
        auto mingw = ensure_mingw_objects(tc);
        rt.libc = mingw.libmingw32_a;
        // Don't inject crt entry — the Clang driver adds crt2.o itself as
        // a bare filename. We just need it in the -L search path (below).
        // Link search paths: cache dir (for crt2.o, libmingw32.a) + import libs.
        if (!mingw.crt2_o.string().empty()) {
            rt.link_dirs.push_back(mingw.crt2_o.parent().string());
            rt.mingw_import_dir = mingw.import_lib_dir.string();
        }
        // Always-link system libraries.
        for (auto* lib : mingw_always_link_libs)
            rt.always_link_libs.push_back(lib);
        break;
    }
    case LibcFamily::None:
        break;
    }

    return rt;
}

} // namespace bake
