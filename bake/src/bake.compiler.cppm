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
            cmd.push_back((find_lib_dir() / "libcxx" / "cross-config").string());
        }

        cmd.push_back("-isystem");
        cmd.push_back(libcxx_inc.string());
        cmd.push_back("-isystem");
        cmd.push_back((find_lib_dir() / "include").string());
        cmd.push_back("-Wno-reserved-module-identifier");
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
            cmd.push_back((find_lib_dir() / "libcxx" / "cross-config").string());
        }

        cmd.push_back("-isystem");
        cmd.push_back(libcxx_inc.string());
        cmd.push_back("-isystem");
        cmd.push_back((find_lib_dir() / "include").string());
        cmd.push_back("-Wno-reserved-module-identifier");
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

// ===== In-process archive writer (replaces system ar) =====
//
// bake embeds LLVM's archive writer, so it never depends on external `ar`.

static bool write_archive(const Path& archive_path,
                          const std::vector<Path>& members,
                          bool is_darwin) {
    std::vector<std::string> paths;
    std::vector<const char *> cstrs;
    paths.reserve(members.size());
    cstrs.reserve(members.size());
    for (auto& m : members) {
        paths.push_back(m.string());
        cstrs.push_back(paths.back().c_str());
    }
    int kind = is_darwin ? 2 : 0;  // 0=GNU, 2=DARWIN
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
    Musl,    // built from vendored source
    Darwin,  // libSystem via vendored .tbd (cross) or system SDK (native)
    None,    // freestanding (no libc)
    // Future: Gnu, Mingw, ...
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
};

export LibcFamily resolve_libc_family(const TargetSpec& target) {
    if (target.is_linux_musl()) return LibcFamily::Musl;
    if (target.is_darwin())     return LibcFamily::Darwin;
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

    bool needs_libunwind = tc.target.is_linux_musl();
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
        bool is_darwin = tc.target.is_darwin();
        write_archive(ar_path, objs, is_darwin);
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
            if (ext != ".c" && ext != ".cpp") continue;
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
        write_archive(result_a, obj_files, is_darwin);
    }

    write_file(sentinel, "");
    return result_a;
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
    case LibcFamily::None:
        break;
    }

    return rt;
}

} // namespace bake
