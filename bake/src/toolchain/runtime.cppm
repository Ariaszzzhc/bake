module;

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

export module bake.toolchain.runtime;

import std;
import bake.util;
import bake.toolchain.target;
import bake.toolchain.lld;

// ============================================================
// bake.toolchain.runtime — vendored runtime provisioning
//
// Builds and caches every runtime product bake compiles with:
// std module PCMs, libc++/libc++abi/libunwind, musl/MinGW/glibc
// CRTs, synthesized glibc stub libraries, compiler-rt and the
// sanitizer runtimes — all from the vendored sources in lib/,
// content-addressed under ~/.cache/bake/<triple>/.
// ============================================================

namespace bake {

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

// ===== Manifest-based toolchain cache (per-triple, content-addressed) =====
//
// Layout under the toolchain cache root:
//
//   <triple-with-version>/
//     h/<config-hash>.txt   manifest: final digest + per-input-file
//                           size/mtime/digest lines
//     o/<final-hash>/       products (one directory per distinct
//                           configuration+input combination)
//
// Two-stage hashing: the CONFIG hash (label + config lines, e.g. compiler
// identity, target versions, flag synopsis) names the manifest; the FINAL
// digest (config hash + every input file's content digest) names the
// product directory. Any change to configuration or input file contents
// produces a new final digest and a plain miss — stale products never
// collide, and no bump markers are needed (vendored-header edits are
// caught by the file digests).

// Root of the global toolchain cache (content-addressed).
// Honours BAKE_CACHE_DIR for testing; defaults to ~/.cache/bake
// (<LOCALAPPDATA>/bake on Windows).
export Path get_toolchain_cache_root() {
    if (const char* env = std::getenv("BAKE_CACHE_DIR"))
        if (env[0] != '\0') return Path(env);
#ifdef _WIN32
    const char* home = std::getenv("LOCALAPPDATA");
    if (!home) home = "C:\\";
    return Path(home) / "bake";
#else
    const char* home = std::getenv("HOME");
    if (!home) home = "/tmp";
    return Path(home) / ".cache" / "bake";
#endif
}

export struct ToolchainCacheEntry {
    Path output_dir;   // o/<final>; created on miss, valid when hit
    bool hit = false;  // manifest matches and output dir exists

    Path manifest_path;                    // h/<config-hash>.txt
    std::vector<std::string> lines;        // manifest content for finish()
};

namespace {

// Expand directories recursively into their regular files.
void collect_input_files(const Path& p, std::vector<Path>& out) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::is_regular_file(p.fs(), ec)) {
        out.push_back(p);
        return;
    }
    if (!fs::is_directory(p.fs(), ec)) return;
    for (auto& e : fs::recursive_directory_iterator(p.fs(), ec))
        if (e.is_regular_file(ec))
            out.emplace_back(e.path());
}

struct FileStamp {
    std::uintmax_t size = 0;
    std::int64_t mtime = 0;
    bool valid = false;
};

FileStamp stat_stamp(const Path& p) {
    namespace fs = std::filesystem;
    std::error_code ec;
    FileStamp s;
    if (auto st = fs::status(p.fs(), ec); fs::exists(st)) {
        s.size = fs::file_size(p.fs(), ec);
        s.mtime = std::int64_t(
            fs::last_write_time(p.fs(), ec).time_since_epoch().count());
        s.valid = !ec;
    }
    return s;
}

} // namespace

ToolchainCacheEntry toolchain_cache_lookup(
        const TargetSpec& target, std::string_view label,
        const std::vector<std::string>& config,
        const std::vector<Path>& inputs) {
    ToolchainCacheEntry entry;

    // Directories: <triple>/h, <triple>/o.
    Path triple_dir =
        get_toolchain_cache_root() / target.triple_with_version();
    Path h_dir = triple_dir / "h";
    Path o_dir = triple_dir / "o";

    std::string config_data(label);
    for (auto& line : config)
        config_data += "\n" + line;
    std::string config_hash = SHA256::hex(config_data).substr(0, 16);

    entry.manifest_path = h_dir / (config_hash + ".txt");

    // Expand inputs (directories → recursive file lists) in stable order.
    std::vector<Path> files;
    for (auto& p : inputs) collect_input_files(p, files);
    std::sort(files.begin(), files.end(),
              [](const Path& a, const Path& b) {
                  return a.string() < b.string();
              });

    // Collect stamps + digests. When a manifest exists, files whose
    // size+mtime match the recorded line are trusted (digest reused,
    // no re-hash) — same trust model as mtime-based manifests.
    std::vector<std::string> new_lines;
    std::string digests;
    bool manifest_exists = entry.manifest_path.is_regular_file();
    std::optional<std::string> existing =
        manifest_exists ? read_file(entry.manifest_path) : std::nullopt;
    std::istringstream in(existing ? *existing : std::string());
    std::string first;
    bool ok = false;
    if (existing) {
        std::getline(in, first);
        ok = !first.empty();
    }

    for (const auto& f : files) {
        FileStamp st = stat_stamp(f);
        std::string size_mtime = st.valid
            ? std::to_string(st.size) + " " + std::to_string(st.mtime)
            : "- -";

        std::string line;
        std::string digest;
        if (ok) {
            if (!std::getline(in, line)) {
                ok = false;                       // manifest shorter
            } else if (line.compare(0, size_mtime.size(), size_mtime) == 0) {
                // size+mtime match → trust the recorded digest.
                auto d1 = line.find(' ', size_mtime.size() + 1);
                auto d2 = line.find(' ', d1 + 1);
                if (d1 != std::string::npos && d2 != std::string::npos)
                    digest = line.substr(d1 + 1, d2 - d1 - 1);
                else
                    ok = false;
            }
        }
        if (digest.empty()) {
            ok = false;                           // stale or missing record
            digest = st.valid ? SHA256::hex_file(f) : std::string("-");
        }
        digests += digest;
        new_lines.push_back(size_mtime + " " + digest + " " + f.string());
    }
    if (ok && in.peek() != std::char_traits<char>::eof())
        ok = false;  // manifest longer than inputs

    if (ok) {
        Path candidate = o_dir / first;
        if (candidate.is_directory()) {
            entry.output_dir = candidate;
            entry.hit = true;
            return entry;
        }
    }

    // Miss: final digest names the product directory; stage it.
    std::string final_hash =
        SHA256::hex(config_hash + "\n" + digests).substr(0, 16);

    entry.lines.push_back(final_hash);
    for (auto& l : new_lines) entry.lines.push_back(l);
    entry.output_dir = o_dir / final_hash;
    entry.output_dir.mkdir_recursive();
    h_dir.mkdir_recursive();
    return entry;
}

void toolchain_cache_finish(ToolchainCacheEntry& entry) {
    if (entry.hit || entry.manifest_path.string().empty()) return;
    std::string content;
    for (auto& l : entry.lines)
        content += l + "\n";
    write_file(entry.manifest_path, content);
}

// Capture compiler identity + version + target triple as a stable string.
// Cached to a file so we avoid spawning the compiler on every invocation.
static std::string compiler_identity_block(const TargetSpec& target) {
    namespace fs = std::filesystem;

    // Include target triple in cache identity so cross-compile keys differ.
    std::string target_suffix = target.is_native() ? "" : "\n" + target.triple();

    // Per-target cache file to avoid native/cross thrashing. Lives under
    // .identity/ (not the triple tree): identity content is identical for
    // every version suffix of a triple, so it must not fan out per-version.
    std::string id_name = ".compiler-identity";
    if (!target.is_native())
        id_name += "-" + target.triple();

    Path identity_dir = get_toolchain_cache_root() / ".identity";
    identity_dir.mkdir_recursive();
    Path identity_cache = identity_dir / id_name;
    Path compiler_bin(bake_exe_path());
    if (identity_cache.is_regular_file() && compiler_bin.is_regular_file()) {
        if (fs::last_write_time(compiler_bin.fs()) <=
            fs::last_write_time(identity_cache.fs())) {
            if (auto cached = read_file(identity_cache))
                return *cached;
        }
    }

    std::vector<std::string> prefix = {bake_exe_path(), "c++"};

    auto ver_args = prefix;
    ver_args.push_back("--version");
    auto ver = run_process(ver_args, Path(), true);

    auto triple_args = prefix;
    triple_args.push_back("-dumpmachine");
    auto triple = run_process(triple_args, Path(), true);

    std::string block = bake_exe_path() + "\n" +
                        ver.stdout_output + "\n" +
                        ver.stderr_output + "\n" +
                        triple.stdout_output + "\n" +
                        target_suffix + "\n";

    write_file(identity_cache, block);
    return block;
}

export struct ToolchainCacheInfo {
    std::string key;
    Path dir;
};

// Target-version lines that shape the compiled std module surface: the
// glibc header-gate version for gnu targets, the deployment minimum for
// darwin. They are config inputs (not file inputs) because they arrive
// via -D injection / the target triple.
static std::vector<std::string> target_surface_lines(const TargetSpec& target) {
    std::vector<std::string> lines;
    if (target.is_linux_gnu())
        lines.push_back("glibc:" + std::to_string(target.glibc_major()) +
                        "." + std::to_string(target.glibc_minor()));
    if (target.is_darwin())
        lines.push_back("macos-min:" + target.macos_deployment_min());
    return lines;
}

static ToolchainCacheEntry std_module_cache_entry(const TargetSpec& target) {
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

    std::string revision;
    if (auto rev_content = read_file(find_lib_dir() / "libcxx" / "LLVM_REVISION"))
        revision = *rev_content;

    std::vector<std::string> config;
    config.push_back("std-modules-v4");
    config.push_back(compiler_identity_block(target));
    config.push_back(std_hash);
    config.push_back(compat_hash);
    config.push_back(revision);
    for (auto& l : target_surface_lines(target)) config.push_back(l);

    // The precompiled module compiles against the libc++ headers and the
    // target's config-site directory — both tracked by content digest.
    std::vector<Path> inputs;
    Path lib = find_lib_dir();
    inputs.push_back(lib / "libcxx" / "include");
    std::string config_subdir = target.is_windows_gnu()
        ? "mingw-config"
        : target.is_linux_gnu() ? "gnu-config" : "cross-config";
    inputs.push_back(lib / "libcxx" / config_subdir);

    return toolchain_cache_lookup(target, "std-modules", config, inputs);
}

static bool atomic_compile_pcm(
        const std::vector<std::string>& compile_cmd,
        const Path& dest) {
    if (dest.is_regular_file()) return true;

    Path parent = dest.parent();
    parent.mkdir_recursive();

    std::string suffix = "." + std::to_string(
#ifdef _WIN32
        _getpid()
#else
        getpid()
#endif
    ) + ".tmp";
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
        const TargetSpec& target, const Path& /*project_out*/) {
    ModuleFileMap result;

    auto entry = std_module_cache_entry(target);

    Path std_pcm = entry.output_dir / "std.pcm";
    Path compat_pcm = entry.output_dir / "std.compat.pcm";

    Path gen_dir = get_toolchain_cache_root() / ".gen";
    Path std_src = gen_dir / "std.cppm";
    Path compat_src = gen_dir / "std.compat.cppm";

    Path libcxx_inc = find_lib_dir() / "libcxx" / "include";

    // Select libc++ config_site based on target: mingw-config for windows-gnu,
    // cross-config for other cross-compile targets.
    std::string config_subdir = target.is_windows_gnu()
        ? "mingw-config"
        : target.is_linux_gnu() ? "gnu-config" : "cross-config";

    if (!std_pcm.is_regular_file()) {
        std::println("   Preparing standard library module");

        std::vector<std::string> cmd = {bake_exe_path(), "c++"};
        cmd.push_back("-std=c++23");
        cmd.push_back("-stdlib=libc++");
        cmd.push_back("-nostdinc++");

        // Cross-compile: -target + cross __config_site before libc++ headers.
        if (!target.is_native()) {
            cmd.push_back("-target");
            cmd.push_back(target.triple_with_version());
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
        std::vector<std::string> cmd = {bake_exe_path(), "c++"};
        cmd.push_back("-std=c++23");
        cmd.push_back("-stdlib=libc++");
        cmd.push_back("-nostdinc++");

        if (!target.is_native()) {
            cmd.push_back("-target");
            cmd.push_back(target.triple_with_version());
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

    toolchain_cache_finish(entry);
    result["std"] = std_pcm;
    result["std.compat"] = compat_pcm;
    return result;
}

export ToolchainCacheInfo bake_build_cache_info(
        const TargetSpec& target, const Path& wrapper_source) {
    auto base = std_module_cache_entry(target);

    std::vector<std::string> config;
    config.push_back("bake.build-v2");
    // The module compiles against the std module's exact surface: chaining
    // its final digest re-keys bake.build whenever std's inputs change.
    config.push_back("std-final:" + base.output_dir.filename().string());
    if (wrapper_source.string().empty() || !wrapper_source.is_regular_file())
        return {"", Path()};

    std::vector<Path> inputs{wrapper_source};
    auto entry = toolchain_cache_lookup(target, "bake.build", config, inputs);
    // The pcm build in cli is existence-checked there; the manifest only
    // provides the isolated directory, so no finish() is needed.
    return {entry.output_dir.filename().string(), entry.output_dir};
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
//   2. prepare_runtime(target, ...) → RuntimeArtifacts (calls ensure_* per family)
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
    Gnu,      // glibc: crt from vendored source, libc via synthesized stubs
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
    std::string macos_sdk_version;             // SDK version (platform_version 4th arg)

    // windows (mingw) link helpers
    std::vector<std::string> always_link_libs; // system libs to inject
    std::string mingw_import_dir;              // path to generated import libs

    std::vector<std::string> gnu_stub_libs;     // full paths, link order
    std::string dynamic_linker;                // PT_INTERP path (ld-linux)
};
LibcFamily resolve_libc_family(const TargetSpec& target) {
    if (target.is_linux_musl())  return LibcFamily::Musl;
    if (target.is_windows_gnu()) return LibcFamily::Windows;
    if (target.is_darwin())      return LibcFamily::Darwin;
    if (target.is_linux_gnu())   return LibcFamily::Gnu;
    return LibcFamily::None;
}

export DarwinSdkLayout resolve_darwin_sdk(const TargetSpec& target) {
    // Native darwin → try system SDK; cross → vendored
    auto host = detect_host_target();
    if (host.is_darwin() && target.is_darwin())
        return DarwinSdkLayout::SystemSdk;
    return DarwinSdkLayout::Vendored;
}

export enum class GnuSdkLayout {
    SystemGnu,  // native glibc host: system /usr/include + /usr/lib directly
    Vendored,   // cross (or non-glibc host): vendored headers + synthesized stubs
};

export GnuSdkLayout resolve_gnu_sdk(const TargetSpec& target) {
    // Only a bake binary linked against the host's glibc (stage-0 build on
    // a glibc distro) and targeting its own arch may use the system libc.
    // Unlike darwin, glibc headers are strictly per-arch: cross-arch from a
    // glibc host still needs the vendored/synthesized layout.
    auto host = detect_host_target();
    if (host.is_linux_gnu() && target.is_linux_gnu() &&
        host.arch() == target.arch())
        return GnuSdkLayout::SystemGnu;
    return GnuSdkLayout::Vendored;
}
export LinkMode parse_link_mode(const std::vector<std::string>& args) {
    bool is_shared = false;
    bool is_static = false;
    bool is_static_pie = false;
    for (auto& a : args) {
        if (a == "-shared" || a == "-Bshareable") is_shared = true;
        if (a == "-static-pie") is_static_pie = true;
        if (a == "-static" || a == "-Bstatic") is_static = true;
    }
    if (is_static_pie) return LinkMode::StaticPie;
    if (is_static && !is_shared) return LinkMode::Static;
    return LinkMode::Dynamic;
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

export CxxRuntime ensure_cxx_runtime(const TargetSpec& target) {
    CxxRuntime result;
    Path lib = find_lib_dir();
    if (lib.string().empty()) return result;

    // Manifest-isolated product directory: keyed by compiler identity +
    // target surface lines + the content of every runtime source/header
    // input. Version changes (glibc gate version, darwin min) and vendored
    // header edits automatically produce a fresh directory.
    std::vector<std::string> config;
    config.push_back("cxx-runtime-v2");
    config.push_back(compiler_identity_block(target));
    for (auto& l : target_surface_lines(target)) config.push_back(l);

    std::vector<Path> inputs;
    for (const char* d : {"libcxx/src", "libcxxabi/src", "libunwind/src",
                          "libcxx/include", "libcxxabi/include",
                          "libunwind/include", "libcxx/libc",
                          "libcxx/cross-config", "libcxx/gnu-config",
                          "libcxx/mingw-config"})
        inputs.push_back(lib / std::string(d));

    auto entry = toolchain_cache_lookup(target, "cxx-runtime", config, inputs);

    result.libcxx_a    = entry.output_dir / "libc++.a";
    result.libcxxabi_a = entry.output_dir / "libc++abi.a";

    bool needs_libunwind = target.is_linux_musl() ||
                           target.is_linux_gnu() ||
                           target.is_windows();
    if (needs_libunwind)
        result.libunwind_a = entry.output_dir / "libunwind.a";

    auto products_ok = [&]() {
        if (!result.libcxx_a.is_regular_file()) return false;
        if (!result.libcxxabi_a.is_regular_file()) return false;
        if (needs_libunwind && !result.libunwind_a.is_regular_file())
            return false;
        return true;
    };
    if (entry.hit && products_ok()) return result;
    if (products_ok()) {  // interrupted after build, before manifest write
        toolchain_cache_finish(entry);
        return result;
    }

    Path cache_dir = entry.output_dir;
    std::println("   Compiling C++ runtime for {} (cached)",
                 target.triple_with_version());

    auto libcxxabi_inc = lib.string() + "/libcxxabi/include";
    auto libcxx_src    = lib.string() + "/libcxx/src";
    auto libcxxabi_src = lib.string() + "/libcxxabi/src";
    auto libcxx_libc   = lib.string() + "/libcxx/libc";
    auto libunwind_inc = lib.string() + "/libunwind/include";
    auto libunwind_src = lib.string() + "/libunwind/src";

    // -target flag: always add when non-native (identical for all targets).
    std::vector<std::string> target_flag;
    if (!target.is_native()) {
        target_flag.push_back("-target");
        target_flag.push_back(target.triple());
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
        write_archive(ar_path, objs, target.is_darwin(), target.is_windows());
    };

    int total = 0;

    // ── libunwind (musl only — darwin uses libSystem) ──
    if (needs_libunwind) {
        std::vector<Path> objs;

        for (auto* f : libunwind_c_files) {
            std::vector<std::string> flags = {
                bake_exe_path(), "cc",
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
                bake_exe_path(), "c++",
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
                bake_exe_path(), "cc",
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
        std::vector<std::string> flags = {bake_exe_path(), "c++"};
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
        std::vector<std::string> flags = {bake_exe_path(), "c++"};
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
        if (target.is_windows()) {
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
    toolchain_cache_finish(entry);
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

export MuslObjects ensure_musl_objects(const TargetSpec& target) {
    MuslObjects result;
    if (!target.is_linux_musl()) return result;

    Path lib = find_lib_dir();
    if (lib.string().empty()) return result;

    Path musl_src = lib / "libc" / "musl";
    std::string arch = target.arch();

    std::vector<std::string> config;
    config.push_back("musl-v2");
    config.push_back(compiler_identity_block(target));

    auto entry = toolchain_cache_lookup(target, "musl", config, {musl_src});
    Path cache_dir = entry.output_dir;

    result.crt1_o  = cache_dir / "crt1.o";
    result.rcrt1_o = cache_dir / "rcrt1.o";
    result.Scrt1_o = cache_dir / "Scrt1.o";
    result.libc_a  = cache_dir / "libc.a";

    auto products_ok = [&]() {
        return result.crt1_o.is_regular_file() &&
               result.libc_a.is_regular_file();
    };
    if (entry.hit && products_ok()) return result;
    if (products_ok()) {
        toolchain_cache_finish(entry);
        return result;
    }

    std::println("   Compiling musl for {} (cached)",
                 target.triple_with_version());

    write_file(cache_dir / "version.h",
               "#define VERSION \"" + std::string("1.2.5") + "\"\n");

    auto make_flags = [&]() {
        std::vector<std::string> flags;
        flags.push_back(bake_exe_path());
        flags.push_back("cc");
        flags.push_back("-target");
        flags.push_back(target.triple());
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
    toolchain_cache_finish(entry);

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

export Path ensure_compiler_rt_objects(const TargetSpec& target) {
    Path lib = find_lib_dir();
    if (lib.string().empty()) return Path();

    Path builtins_dir = lib / "compiler-rt" / "lib" / "builtins";

    std::vector<std::string> config;
    config.push_back("compiler-rt-v2");
    config.push_back(compiler_identity_block(target));

    auto entry = toolchain_cache_lookup(target, "compiler-rt", config,
                                        {builtins_dir});
    Path cache_dir = entry.output_dir;
    Path result_a = cache_dir / "libcompiler_rt.a";

    if (entry.hit && result_a.is_regular_file())
        return result_a;
    if (result_a.is_regular_file()) {
        toolchain_cache_finish(entry);
        return result_a;
    }

    bool is_darwin = target.is_darwin();
    std::string arch = target.arch();

    auto make_flags = [&]() {
        std::vector<std::string> flags;
        flags.push_back(bake_exe_path());
        flags.push_back("cc");
        if (!target.is_native()) {
            flags.push_back("-target");
            flags.push_back(target.triple());
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
        write_archive(result_a, obj_files, is_darwin, target.is_windows());
    }

    toolchain_cache_finish(entry);
    return result_a;
}

// ── Sanitizer runtimes (ubsan standalone + asan), built from vendored
//    compiler-rt sources per ELF target (linux-gnu, linux-musl) ──

export enum class SanitizerKind {
    Ubsan,
    Asan,
};

namespace {

// sanitizer_common core + libcdep + symbolizer sets shared by every
// runtime. Files absent from the vendored tree are skipped.
const char* sanitizer_common_sources[] = {
    "sanitizer_allocator.cpp", "sanitizer_chained_origin_depot.cpp",
    "sanitizer_common.cpp", "sanitizer_deadlock_detector1.cpp",
    "sanitizer_deadlock_detector2.cpp", "sanitizer_errno.cpp",
    "sanitizer_file.cpp", "sanitizer_flag_parser.cpp",
    "sanitizer_flags.cpp", "sanitizer_fuchsia.cpp", "sanitizer_haiku.cpp",
    "sanitizer_libc.cpp", "sanitizer_libignore.cpp", "sanitizer_linux.cpp",
    "sanitizer_linux_s390.cpp", "sanitizer_mac.cpp", "sanitizer_mutex.cpp",
    "sanitizer_netbsd.cpp", "sanitizer_platform_limits_freebsd.cpp",
    "sanitizer_platform_limits_linux.cpp",
    "sanitizer_platform_limits_netbsd.cpp",
    "sanitizer_platform_limits_posix.cpp",
    "sanitizer_platform_limits_solaris.cpp", "sanitizer_posix.cpp",
    "sanitizer_printf.cpp", "sanitizer_procmaps_bsd.cpp",
    "sanitizer_procmaps_common.cpp", "sanitizer_procmaps_fuchsia.cpp",
    "sanitizer_procmaps_haiku.cpp", "sanitizer_procmaps_linux.cpp",
    "sanitizer_procmaps_mac.cpp", "sanitizer_procmaps_solaris.cpp",
    "sanitizer_range.cpp", "sanitizer_solaris.cpp",
    "sanitizer_stoptheworld_fuchsia.cpp", "sanitizer_stoptheworld_mac.cpp",
    "sanitizer_stoptheworld_win.cpp", "sanitizer_suppressions.cpp",
    "sanitizer_termination.cpp", "sanitizer_thread_arg_retval.cpp",
    "sanitizer_thread_registry.cpp", "sanitizer_tls_get_addr.cpp",
    "sanitizer_type_traits.cpp", "sanitizer_win.cpp",
    "sanitizer_win_interception.cpp",
};
const char* sanitizer_libcdep_sources[] = {
    "sanitizer_common_libcdep.cpp", "sanitizer_allocator_checks.cpp",
    "sanitizer_dl.cpp", "sanitizer_linux_libcdep.cpp",
    "sanitizer_mac_libcdep.cpp", "sanitizer_posix_libcdep.cpp",
    "sanitizer_stoptheworld_linux_libcdep.cpp",
    "sanitizer_stoptheworld_netbsd_libcdep.cpp",
};
const char* sanitizer_symbolizer_sources[] = {
    "sanitizer_allocator_report.cpp", "sanitizer_stack_store.cpp",
    "sanitizer_stackdepot.cpp", "sanitizer_stacktrace.cpp",
    "sanitizer_stacktrace_libcdep.cpp", "sanitizer_stacktrace_printer.cpp",
    "sanitizer_stacktrace_sparc.cpp", "sanitizer_symbolizer.cpp",
    "sanitizer_symbolizer_libbacktrace.cpp", "sanitizer_symbolizer_libcdep.cpp",
    "sanitizer_symbolizer_mac.cpp", "sanitizer_symbolizer_markup.cpp",
    "sanitizer_symbolizer_markup_fuchsia.cpp",
    "sanitizer_symbolizer_posix_libcdep.cpp", "sanitizer_symbolizer_report.cpp",
    "sanitizer_symbolizer_report_fuchsia.cpp", "sanitizer_symbolizer_win.cpp",
    "sanitizer_thread_history.cpp", "sanitizer_unwind_linux_libcdep.cpp",
    "sanitizer_unwind_fuchsia.cpp", "sanitizer_unwind_win.cpp",
};
const char* interception_sources[] = {
    "interception_linux.cpp", "interception_mac.cpp",
    "interception_win.cpp", "interception_type_test.cpp",
};

const char* ubsan_standalone_sources[] = {
    "ubsan_diag.cpp", "ubsan_diag_standalone.cpp", "ubsan_flags.cpp",
    "ubsan_handlers.cpp", "ubsan_handlers_cxx.cpp", "ubsan_init.cpp",
    "ubsan_init_standalone.cpp", "ubsan_init_standalone_preinit.cpp",
    "ubsan_monitor.cpp", "ubsan_signals_standalone.cpp",
    "ubsan_type_hash.cpp", "ubsan_type_hash_itanium.cpp",
    "ubsan_value.cpp",
};

// asan core (upstream ASAN_SOURCES) plus the leak-detection common core it
// always embeds (RTLSanCommon) and the ubsan reporting core (RTUbsan,
// non-apple set).
const char* asan_core_sources[] = {
    "asan_allocator.cpp", "asan_activation.cpp", "asan_debugging.cpp",
    "asan_descriptions.cpp", "asan_errors.cpp", "asan_fake_stack.cpp",
    "asan_flags.cpp", "asan_globals.cpp", "asan_interceptors.cpp",
    "asan_interceptors_memintrinsics.cpp", "asan_linux.cpp",
    "asan_malloc_linux.cpp", "asan_memory_profile.cpp",
    "asan_new_delete.cpp", "asan_poisoning.cpp", "asan_posix.cpp",
    "asan_premap_shadow.cpp", "asan_report.cpp", "asan_rtl.cpp",
    "asan_shadow_setup.cpp", "asan_stack.cpp", "asan_stats.cpp",
    "asan_suppressions.cpp", "asan_thread.cpp",
};
const char* lsan_common_sources[] = {
    "lsan_common.cpp", "lsan_common_fuchsia.cpp",
    "lsan_common_linux.cpp", "lsan_common_mac.cpp",
};
// The ubsan reporting core shared by tsan and asan (upstream UBSAN_SOURCES;
// the standalone/cxx/type-hash units are runtime-specific).
const char* ubsan_core_sources[] = {
    "ubsan_diag.cpp", "ubsan_flags.cpp", "ubsan_handlers.cpp",
    "ubsan_init.cpp", "ubsan_monitor.cpp", "ubsan_value.cpp",
};

// SDK version from the vendored SDKSettings.json ("MinimalDisplayName").
// Empty when the file is absent — callers fall back to the deployment
// minimum.
std::string darwin_vendored_sdk_version(const Path& lib) {
    if (auto content = read_file(lib / "libc" / "darwin" /
                                 "SDKSettings.json")) {
        auto key = std::string("\"MinimalDisplayName\":\"");
        auto pos = content->find(key);
        if (pos != std::string::npos) {
            auto start = pos + key.size();
            auto end = content->find('"', start);
            if (end != std::string::npos)
                return content->substr(start, end - start);
        }
    }
    return std::string();
}

// Darwin link surface — the single place deciding where libSystem and the
// SDK version come from. Native builds use the system SDK via xcrun (like
// the platform toolchain); cross builds — and any xcrun failure — use the
// vendored stub set, whose libSystem.tbd is an equivalent export table.
struct DarwinLinkFace {
    std::string lib_dir;        // -L directory holding libSystem
    std::string framework_dir;  // system frameworks ("" for vendored)
    std::string sdk_version;    // -platform_version's 4th argument
};

DarwinLinkFace resolve_darwin_link_face(const TargetSpec& target) {
    DarwinLinkFace f;
    Path lib = find_lib_dir();
    std::string vendored = (lib / "libc" / "darwin").string();
    f.sdk_version = target.macos_deployment_min();  // last resort

    if (resolve_darwin_sdk(target) == DarwinSdkLayout::SystemSdk) {
        auto run_trimmed = [](const char* what) {
            std::string out;
            auto r = run_process({"xcrun", "--sdk", "macosx", what},
                                 Path(), true);
            if (r.success()) {
                out = r.stdout_output;
                while (!out.empty() && (out.back() == '\n' ||
                                        out.back() == '\r'))
                    out.pop_back();
            }
            return out;
        };
        std::string sdk_path = run_trimmed("--show-sdk-path");
        if (!sdk_path.empty()) {
            f.lib_dir = sdk_path + "/usr/lib";
            f.framework_dir = sdk_path + "/System/Library/Frameworks";
            std::string v = run_trimmed("--show-sdk-version");
            if (!v.empty()) f.sdk_version = v;
            return f;
        }
    }
    f.lib_dir = vendored;
    if (auto v = darwin_vendored_sdk_version(lib); !v.empty())
        f.sdk_version = v;
    return f;
}

// Undefined references the official darwin sanitizer dylibs ship with:
//  - cxxabi/typeinfo, resolved against the main executable's C++ ABI
//  - operator new/delete, the __DATA,__interpose originals — dyld
//    reroutes calls made through stubs into the runtime's wrappers
// -U permits exactly these; anything else fails at link time, so a hole
// in the source list (a missing platform file) surfaces when the dylib
// is built, not when the user runs their program.
const char* darwin_dylib_undef_allowances[] = {
    "___cxa_atexit", "___cxa_demangle",
    "___cxa_rethrow_primary_exception", "___cxa_throw", "___dynamic_cast",
    "__ZTIN10__cxxabiv117__class_type_infoE",
    "__ZTIN10__cxxabiv120__si_class_type_infoE",
    "__ZTIN10__cxxabiv121__vmi_class_type_infoE",
    "__ZTISt9type_info",
    "__ZTVN10__cxxabiv117__class_type_infoE",
    "__ZTVN10__cxxabiv120__si_class_type_infoE",
    "__Znwm", "__ZnwmRKSt9nothrow_t", "__Znam", "__ZnamRKSt9nothrow_t",
    "__ZdlPv", "__ZdlPvRKSt9nothrow_t", "__ZdaPv", "__ZdaPvRKSt9nothrow_t",
};

// Per-directory weak-reference allowances the dylib may leave
// undefined (hooks the user's program may define; weak SDK imports).
// These lists are vendored next to the sources — upstream uses the same
// lists when linking the official darwin dylibs.
std::vector<std::string> load_weak_symbols(const Path& dir) {
    std::vector<std::string> syms;
    auto content = read_file(dir / "weak_symbols.txt");
    if (!content) return syms;
    std::size_t pos = 0;
    while (pos < content->size()) {
        std::size_t eol = content->find('\n', pos);
        if (eol == std::string::npos) eol = content->size();
        std::string_view line(content->data() + pos, eol - pos);
        pos = eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.remove_suffix(1);
        if (!line.empty() && line[0] != '#' && line[0] != ';')
            syms.emplace_back(line);
    }
    return syms;
}
 
// Link a sanitizer dylib with the in-process Mach-O driver. The
// -install_name is @rpath-based: the driver injects the cache-directory
// rpath into sanitized links, matching the official Clang layout.
bool link_darwin_sanitizer_dylib(const TargetSpec& target, const Path& rt_root,
                                 SanitizerKind kind, const Path& out,
                                 const std::vector<Path>& objs,
                                 const std::string& soname) {
    auto face = resolve_darwin_link_face(target);
    std::string arch = target.arch() == "x86_64" ? "x86_64" : "arm64";
    std::string min_v = target.macos_deployment_min();

    std::vector<std::string> args;
    args.push_back("ld64.lld");
    args.push_back("-dylib");
    args.push_back("-arch");
    args.push_back(arch);
    args.push_back("-platform_version");
    args.push_back("macos");
    args.push_back(min_v);
    args.push_back(face.sdk_version);
    args.push_back("-install_name");
    args.push_back("@rpath/" + soname);
    args.push_back("-o");
    args.push_back(out.string());
    for (auto& o : objs) args.push_back(o.string());
    // Fixed cxxabi/interpose allowances, plus every vendored weak list of
    // the runtime families the dylib embeds (asan pulls ubsan+lsan+common).
    std::vector<std::string> und = {std::begin(darwin_dylib_undef_allowances),
                                    std::end(darwin_dylib_undef_allowances)};
    auto append_dir = [&](const char* d) {
        for (auto& s : load_weak_symbols(rt_root / d)) und.push_back(s);
    };
    if (kind == SanitizerKind::Asan) {
        for (auto* d : {"sanitizer_common", "asan", "ubsan", "lsan"})
            append_dir(d);
    } else {
        for (auto* d : {"sanitizer_common", "ubsan"}) append_dir(d);
    }
    for (auto& s : und) {
        args.push_back("-U");
        args.push_back(s);
    }
    args.push_back("-L" + face.lib_dir);
    args.push_back("-lSystem");

    std::vector<const char*> cargs;
    cargs.reserve(args.size());
    for (auto& s : args) cargs.push_back(s.c_str());
    return bake_lld_link(LldFlavor::MACHO, static_cast<int>(cargs.size()),
                         cargs.data()) == 0;
}

// Link the asan runtime DLL with the in-process MinGW driver and produce
// its import library. import_libs carry the win32 APIs the runtime
// references (full paths into the mingw cache).
bool link_mingw_asan_dll(const TargetSpec& target, const Path& dll,
                         const Path& implib,
                         const std::vector<Path>& objs,
                         const std::vector<Path>& import_libs) {
    std::vector<std::string> args;
    args.push_back("ld.lld");
    args.push_back("-m");
    args.push_back("i386pep");  // x86_64 PE (only supported mingw asan arch)
    args.push_back("-shared");
    args.push_back("--out-implib");
    args.push_back(implib.string());
    args.push_back("-o");
    args.push_back(dll.string());
    for (auto& o : objs) args.push_back(o.string());
    for (auto& l : import_libs) args.push_back(l.string());

    std::vector<const char*> cargs;
    cargs.reserve(args.size());
    for (auto& s : args) cargs.push_back(s.c_str());
    return bake_lld_link(LldFlavor::MinGW, static_cast<int>(cargs.size()),
                         cargs.data()) == 0;
}

// win32 import libraries the sanitizer DLL references — defined after
// ensure_mingw_objects (needs the mingw cache layout).
std::vector<Path> mingw_sanitizer_import_libs(const TargetSpec& target);
 
} // namespace

export Path ensure_sanitizer_objects(const TargetSpec& target, SanitizerKind kind) {
    // Runtimes bake builds from the vendored compiler-rt sources, in each
    // platform's official form (what the Clang driver links by default):
    //   ELF (linux-gnu/musl): static archives, whole-archive'd by the driver
    //   darwin:               shared dylibs with @rpath install names
    //   windows-gnu:          ubsan static archive, asan shared DLL
    // Native builds are the contract; cross-built products are unverified.
    bool elf = target.is_linux_gnu() || target.is_linux_musl();
    bool darwin = target.is_darwin();
    bool mingw = target.is_windows_gnu();
    if (!elf && !darwin && !mingw) return Path();
    // Upstream compiler-rt supports asan on x86_64 windows-gnu only
    // (other mingw arches don't ship a working runtime).
    if (mingw && kind == SanitizerKind::Asan &&
        target.arch() != "x86_64") {
        std::println(std::cerr,
            "bake: -fsanitize=address is not available on {} "
            "(compiler-rt supports x86_64 windows-gnu only)",
            target.triple());
        return Path();
    }

    Path lib = find_lib_dir();
    if (lib.string().empty()) return Path();

    Path rt_root = lib / "compiler-rt" / "lib";
    Path common_dir = rt_root / "sanitizer_common";
    if (!common_dir.is_directory()) return Path();

    bool shared = darwin || (mingw && kind == SanitizerKind::Asan);
    std::string product;
    if (kind == SanitizerKind::Ubsan)
        product = darwin ? "libclang_rt.ubsan_osx_dynamic.dylib"
                         : "libclang_rt.ubsan_standalone.a";
    else
        product = darwin ? "libclang_rt.asan_osx_dynamic.dylib"
                : mingw ? "libclang_rt.asan.dll"
                        : "libclang_rt.asan.a";
    // mingw asan: the driver links against the import library; the DLL
    // (its sibling) is copied next to the executable by the driver —
    // windows resolves it from the exe directory.
    std::string link_name = product;
    if (mingw && kind == SanitizerKind::Asan) link_name += ".a";

    const char* label =
        kind == SanitizerKind::Ubsan ? "san-ubsan" : "san-asan";

    // (source directory, file) pairs to compile — assembled BEFORE the
    // cache lookup so the unit list itself is a config input: changing
    // the list re-keys the products automatically.
    std::vector<std::pair<Path, const char*>> units;
    auto add = [&](const Path& dir, const char* const* files, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i)
            units.emplace_back(dir, files[i]);
    };
    auto add_ubsan_standalone = [&]() {
        // ubsan_init_standalone_preinit rides .preinit_array — ELF only.
        for (auto* f : ubsan_standalone_sources)
            if (elf || std::string_view(f) !=
                           "ubsan_init_standalone_preinit.cpp")
                units.emplace_back(rt_root / "ubsan", f);
    };
    add(common_dir, sanitizer_common_sources,
        std::size(sanitizer_common_sources));
    add(common_dir, sanitizer_libcdep_sources,
        std::size(sanitizer_libcdep_sources));
    add(common_dir, sanitizer_symbolizer_sources,
        std::size(sanitizer_symbolizer_sources));
    if (kind == SanitizerKind::Asan) {
        add(rt_root / "asan", asan_core_sources,
            std::size(asan_core_sources));
        if (darwin) {
            units.emplace_back(rt_root / "asan", "asan_mac.cpp");
            units.emplace_back(rt_root / "asan", "asan_malloc_mac.cpp");
        }
        if (mingw) {
            units.emplace_back(rt_root / "asan", "asan_win.cpp");
            units.emplace_back(rt_root / "asan", "asan_globals_win.cpp");
            units.emplace_back(rt_root / "asan", "asan_malloc_win.cpp");
        }
        // Leak detection is built into the asan runtime (RTLSanCommon).
        add(rt_root / "lsan", lsan_common_sources,
            std::size(lsan_common_sources));
        // The ubsan reporting core asan reports through. darwin embeds
        // the apple set (adds the itanium cxx handlers, needs RTTI).
        add(rt_root / "ubsan", ubsan_core_sources,
            std::size(ubsan_core_sources));
        if (darwin) {
            units.emplace_back(rt_root / "ubsan",
                               "ubsan_handlers_cxx.cpp");
            units.emplace_back(rt_root / "ubsan", "ubsan_type_hash.cpp");
            units.emplace_back(rt_root / "ubsan",
                               "ubsan_type_hash_itanium.cpp");
        }
        add(rt_root / "interception", interception_sources,
            std::size(interception_sources));
        if (elf) {
            // Early __asan_init via .preinit_array, plus the vfork
            // interceptor assembly upstream appends on ELF.
            units.emplace_back(rt_root / "asan", "asan_preinit.cpp");
            units.emplace_back(rt_root / "asan",
                               "asan_interceptors_vfork.S");
        }
        units.emplace_back(common_dir, "sanitizer_coverage_libcdep_new.cpp");
        units.emplace_back(common_dir, "sancov_flags.cpp");
    } else {
        add_ubsan_standalone();
        if (mingw)
            // static-rt interface registration for the COFF runtime
            units.emplace_back(rt_root / "ubsan",
                               "ubsan_win_runtime_thunk.cpp");
        add(rt_root / "interception", interception_sources,
            std::size(interception_sources));
        // ubsan_init references the coverage initialization hooks.
        units.emplace_back(common_dir, "sanitizer_coverage_libcdep_new.cpp");
        units.emplace_back(common_dir, "sancov_flags.cpp");
    }

    // Flags shared by every unit (self-contained C++; no standard headers).
    // Built before the cache lookup: every compile flag is a config input,
    // so a flag change re-keys the products (a stale artifact with the old
    // flags can never be picked up).
    auto make_flags = [&]() {
        std::vector<std::string> flags;
        flags.push_back(bake_exe_path());
        flags.push_back("c++");
        flags.push_back("-target");
        flags.push_back(target.triple_with_version());
        flags.push_back("-c");
        flags.push_back("-std=c++17");
        flags.push_back("-nostdinc++");
        // Loop-idiom recognition at -O2 would rewrite hand-rolled
        // internal_strlen/internal_memcpy loops into PLT calls to the
        // intercepted libc names — inside the runtime that re-enters
        // the interceptors before init completes and deadlocks.
        // Upstream compiler-rt compiles runtimes with -fno-builtin.
        flags.push_back("-fno-builtin");
        // The itanium type-hash units use dynamic_cast and need RTTI:
        // every ubsan build, and the darwin asan dylib (it embeds the
        // apple ubsan cxx set). Everything else is compiled without.
        if (kind == SanitizerKind::Ubsan || darwin)
            flags.push_back("-frtti");
        else
            flags.push_back("-fno-rtti");
        if (kind == SanitizerKind::Asan && (darwin || mingw)) {
            // shared-runtime build (upstream ASAN_DYNAMIC_DEFINITIONS);
            // the darwin dylib pins initial-exec TLS like the official one
            flags.push_back("-DASAN_DYNAMIC=1");
            if (darwin) flags.push_back("-ftls-model=initial-exec");
        }
        if (mingw && kind == SanitizerKind::Ubsan)
            flags.push_back("-DSANITIZER_DYNAMIC_RUNTIME_THUNK"),
            flags.push_back("-DSANITIZER_STATIC_RUNTIME_THUNK");
        flags.push_back("-fno-exceptions");
        flags.push_back("-fvisibility=hidden");
        flags.push_back("-fvisibility-inlines-hidden");
        flags.push_back("-fPIC");
        flags.push_back("-O2");
        flags.push_back("-w");
        flags.push_back("-I" + rt_root.string());
        flags.push_back("-I" + common_dir.string());
        flags.push_back("-I" + (rt_root / "interception").string());
        if (kind == SanitizerKind::Asan)
            flags.push_back("-I" + (rt_root / "asan").string());
        else
            flags.push_back("-I" + (rt_root / "ubsan").string());
        return flags;
    };
    auto base_flags = make_flags();

    std::vector<std::string> config;
    config.push_back(kind == SanitizerKind::Ubsan ? "san-ubsan-v4"
                                                  : "san-asan-v4");
    config.push_back("product:" + product);
    config.push_back(compiler_identity_block(target));
    for (auto& l : target_surface_lines(target)) config.push_back(l);
    std::string unit_list;
    for (auto& [dir, file] : units)
        unit_list += std::string(file) + " ";
    config.push_back("units:" + unit_list);
    for (auto& f : base_flags) config.push_back("flag:" + f);

    std::vector<Path> inputs{common_dir, rt_root / "interception"};
    if (kind == SanitizerKind::Ubsan) {
        inputs.push_back(rt_root / "ubsan");
    } else {
        inputs.push_back(rt_root / "asan");
        inputs.push_back(rt_root / "lsan");
        // the ubsan reporting core is embedded in the asan runtime
        inputs.push_back(rt_root / "ubsan");
    }

    auto entry = toolchain_cache_lookup(target, label, config, inputs);
    Path cache_dir = entry.output_dir;
    Path result = cache_dir / link_name;

    if (entry.hit && result.is_regular_file()) return result;
    if (result.is_regular_file()) {
        toolchain_cache_finish(entry);
        return result;
    }

    std::println("   Compiling {} runtime for {} (cached)",
                 kind == SanitizerKind::Ubsan ? "ubsan" : "asan",
                 target.triple_with_version());

    std::vector<Path> obj_files;
    int compiled = 0;
    for (auto& [dir, file] : units) {
        Path src = dir / file;
        if (!src.is_regular_file()) continue;  // tree variation tolerance
        std::string stem(file);
        stem = stem.substr(0, stem.rfind('.'));
        Path obj = cache_dir / (stem + ".o");
        if (!obj.is_regular_file()) {
            auto cmd = base_flags;
            cmd.push_back(src.string());
            cmd.push_back("-o");
            cmd.push_back(obj.string());
            auto r = run_process(cmd, Path(), true);
            if (!r.success()) {
                std::print(std::cerr, "{}", r.stderr_output);
                std::println(std::cerr, "bake: failed to compile {}", file);
                return Path();
            }
            ++compiled;
        }
        obj_files.push_back(obj);
    }

    if (obj_files.empty()) return Path();
    bool ok = false;
    if (shared && darwin) {
        ok = link_darwin_sanitizer_dylib(target, rt_root, kind,
                                         cache_dir / product, obj_files,
                                         product);
        if (!ok)
            std::println(std::cerr, "bake: failed to link {}", product);
    } else if (shared) {  // mingw asan DLL + import library
        Path dll = cache_dir / product;
        ok = link_mingw_asan_dll(target, dll, result, obj_files,
                                 mingw_sanitizer_import_libs(target));
        if (!ok)
            std::println(std::cerr, "bake: failed to link {}", product);
    } else {
        ok = write_archive(result, obj_files, false, mingw);
        if (!ok)
            std::println(std::cerr, "bake: failed to create {}", product);
    }
    if (!ok) return Path();
    std::println("   Compiled {} runtime sources", compiled);
    toolchain_cache_finish(entry);
    return result;
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

// Source file lists — curated from MinGW-w64 (minimal CRT set).

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
    "misc/__p___winitenv.c", "misc/_assert.c", "misc/_onexit.c", "misc/ucrt-access.c",
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

// Shared lookup for the mingw product directory (CRT + libmingw32.a +
// lazily generated import libraries). Both ensure_mingw_objects and
// ensure_mingw_import_lib resolve to the same directory.
static ToolchainCacheEntry mingw_cache_entry(const TargetSpec& target,
                                             const Path& mingw_src) {
    std::vector<std::string> config;
    config.push_back("mingw-v2");
    config.push_back(compiler_identity_block(target));
    return toolchain_cache_lookup(target, "mingw", config, {mingw_src});
}

export MingwObjects ensure_mingw_objects(const TargetSpec& target) {
    MingwObjects result;
    if (!target.is_windows_gnu()) return result;

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

    auto entry = mingw_cache_entry(target, mingw_src);
    Path cache_dir = entry.output_dir;

    result.import_lib_dir = cache_dir / "implib";
    result.crt2_o       = cache_dir / "crt2.o";
    result.dllcrt2_o    = cache_dir / "dllcrt2.o";
    result.libmingw32_a = cache_dir / "libmingw32.a";

    auto products_ok = [&]() {
        return result.crt2_o.is_regular_file() &&
               result.libmingw32_a.is_regular_file();
    };
    if (entry.hit && products_ok()) return result;
    if (products_ok()) {
        toolchain_cache_finish(entry);
        return result;
    }

    result.import_lib_dir.mkdir_recursive();
    std::println("   Compiling mingw-w64 for {} (cached)",
                 target.triple_with_version());

    std::string arch = target.arch();
    std::string machine = (arch == "x86_64") ? "X64" : "ARM64";
    std::string def_arch_dir = (arch == "x86_64") ? "lib64" : "libarm64";

    // ── Common compile flags ──
    auto make_base_flags = [&]() {
        std::vector<std::string> flags;
        flags.push_back(bake_exe_path());
        flags.push_back("cc");
        flags.push_back("-target");
        flags.push_back(target.triple());
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
    toolchain_cache_finish(entry);
    return result;
}

namespace {
// win32 import libraries the sanitizer DLL references (win32 APIs the
// runtime calls). All are always-link mingw libraries with cached
// import libraries.
std::vector<Path> mingw_sanitizer_import_libs(const TargetSpec& target) {
    std::vector<Path> libs;
    auto mw = ensure_mingw_objects(target);
    if (mw.import_lib_dir.string().empty()) return libs;
    for (auto* n : mingw_always_link_libs) {
        Path p = mw.import_lib_dir / (std::string(n) + ".lib");
        if (p.is_regular_file()) libs.push_back(p);
    }
    return libs;
}
} // namespace

// ── On-demand import library generation ──
//
// Called from the driver for each -l<name> on a windows-gnu link line.
// If <name>.lib already exists in the cache, returns immediately.
// Otherwise finds <name>.def in the MinGW source, filters F_* arch directives,
// and generates the import library via LLD COFF driver.
//
// uuid is special: no .def file. It's a static archive of GUID definitions
// compiled from libsrc/*-uuid.c.

export Path ensure_mingw_import_lib(const TargetSpec& target,
                                     const std::string& lib_name) {
    if (!target.is_windows_gnu()) return Path();

    Path lib_dir = find_lib_dir();
    if (lib_dir.string().empty()) return Path();

    Path mingw_src = lib_dir / "libc" / "mingw";
    Path import_dir = mingw_cache_entry(target, mingw_src).output_dir / "implib";

    Path final_lib = import_dir / (lib_name + ".lib");
    if (final_lib.is_regular_file()) return final_lib;
    if (!import_dir.is_directory()) return Path();

    std::string arch = target.arch();
    std::string machine = (arch == "x86_64") ? "X64" : "ARM64";
    std::string def_arch_dir = (arch == "x86_64") ? "lib64" : "libarm64";

    // uuid: static library from C sources (GUID definitions, not DLL import).
    if (lib_name == "uuid") {
        Path libsrc = mingw_src / "libsrc";
        if (!libsrc.is_directory()) return Path();

        std::vector<std::string> uuid_src;
        for (auto& e : std::filesystem::directory_iterator(libsrc.string())) {
            auto name = e.path().filename().string();
            if (name.ends_with("-uuid.c"))
                uuid_src.push_back(e.path().string());
        }
        if (uuid_src.empty()) return Path();

        std::vector<std::string> flags;
        flags.push_back(bake_exe_path());
        flags.push_back("cc");
        flags.push_back("-target");
        flags.push_back(target.triple());
        flags.push_back("-c");
        flags.push_back("-std=gnu11");
        flags.push_back("-D__USE_MINGW_ANSI_STDIO=0");
        flags.push_back("-D__MSVCRT_VERSION__=0x700");
        flags.push_back("-D_CTYPE_DISABLE_MACROS");
        flags.push_back("-D_CRTBLD");
        flags.push_back("-D_SYSCRT=1");
        flags.push_back("-D_WIN32_WINNT=0x0f00");
        flags.push_back("-DCRTDLL=1");
        flags.push_back("-isystem");
        flags.push_back((lib_dir / "libc" / "include" / "any-windows-any").string());
        flags.push_back("-I" + (mingw_src / "include").string());
        flags.push_back("-Os");
        flags.push_back("-w");

        std::vector<Path> obj_paths;
        for (auto& src : uuid_src) {
            std::string fname = std::filesystem::path(src).filename().string();
            std::string obj_name = "uuid__" + fname;
            obj_name.replace(obj_name.size() - 2, 2, ".o");
            Path obj = import_dir / obj_name;
            if (!obj.is_regular_file()) {
                std::vector<std::string> f = flags;
                f.push_back(src);
                f.push_back("-o");
                f.push_back(obj.string());
                auto r = run_process(f, Path(), true);
                if (!r.success()) {
                    std::print(std::cerr, "{}", r.stderr_output);
                    continue;
                }
            }
            obj_paths.push_back(obj);
        }
        if (obj_paths.empty()) return Path();
        write_archive(final_lib, obj_paths, false, true);
        return final_lib;
    }

    // Standard import library: find .def, filter F_* directives, generate.
    Path def_file = mingw_src / def_arch_dir / (lib_name + ".def");
    if (!def_file.is_regular_file())
        def_file = mingw_src / "lib-common" / (lib_name + ".def");
    if (!def_file.is_regular_file()) return Path();

    // Filter F_* arch directives that LLD can't parse.
    Path use_def = def_file;
    {
        std::ifstream in(def_file.string());
        std::string content((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
        if (content.find("F_") != std::string::npos) {
            std::string keep_prefix;
            if (machine == "X64")        keep_prefix = "F_X64(";
            else if (machine == "ARM64") keep_prefix = "F_ARM64(";
            else                         keep_prefix = "F_I386(";

            std::string filtered;
            filtered.reserve(content.size());
            std::size_t pos = 0;
            while (pos < content.size()) {
                std::size_t eol = content.find('\n', pos);
                if (eol == std::string::npos) eol = content.size();
                std::string_view line(content.data() + pos, eol - pos);

                if (line.starts_with("F_")) {
                    bool keep = false;
                    if (line.starts_with(keep_prefix)) keep = true;
                    else if (line.starts_with("F_NON_I386(") &&
                             machine != "X86") keep = true;
                    if (keep) {
                        std::size_t s = line.find('(');
                        std::size_t e = line.find(')', s);
                        if (s != std::string_view::npos &&
                            e != std::string_view::npos)
                            filtered += std::string(line.substr(s + 1, e - s - 1));
                        filtered += '\n';
                    }
                } else {
                    filtered += std::string(line);
                    filtered += '\n';
                }
                pos = eol + 1;
            }

            use_def = import_dir / (lib_name + ".filtered.def");
            write_file(use_def, filtered);
        }
    }

    std::vector<const char*> lld_args;
    lld_args.push_back("lld-link");
    std::string def_arg = "/def:" + use_def.string();
    std::string out_arg = "/out:" + final_lib.string();
    std::string mach_arg = "/machine:" + machine;
    lld_args.push_back(def_arg.c_str());
    lld_args.push_back(out_arg.c_str());
    lld_args.push_back(mach_arg.c_str());

    if (bake_lld_link(LldFlavor::COFF,
                      static_cast<int>(lld_args.size()),
                      lld_args.data()) != 0)
        return Path();

    // LLD creates "lib<name>.lib" — rename to "<name>.lib".
    Path lld_output = import_dir / ("lib" + lib_name + ".lib");
    if (lld_output.is_regular_file() && !final_lib.is_regular_file()) {
        std::error_code ec;
        std::filesystem::rename(lld_output.string(),
                                final_lib.string(), ec);
    }

    return final_lib.is_regular_file() ? final_lib : Path();
}

// ── glibc (linux-gnu): crt + libc_nonshared from vendored source ──
//
// The libc itself is never built: link-time stubs are synthesized from the
// vendored abilists (ensure_glibc_stubs) and real glibc on the target
// machine provides the implementations. glibc 2.28 is the header/source
// baseline: targets below it are rejected (use musl for older baselines).

export struct GlibcObjects {
    Path crt_entry;         // Scrt1.o (old-form start; works for all >= floor)
    Path libc_nonshared_a;
    Path stub_dir;          // lib<name>.so.<sover> stub libraries
    std::string dynamic_linker;
};

static const char* glibc_nonshared_sources[] = {
    "csu/elf-init.c",
    "stdlib/atexit.c",
    "stdlib/at_quick_exit.c",
    "nptl/pthread_atfork.c",
    "debug/stack_chk_fail_local.c",
    "io/stat.c", "io/fstat.c", "io/lstat.c",
    "io/stat64.c", "io/fstat64.c", "io/lstat64.c",
    "io/fstatat.c", "io/fstatat64.c",
    "io/mknod.c", "io/mknodat.c",
};

// Internal include chain for compiling glibc's own sources (mirrors the
// upstream sysdirs order for arch-linux-gnu, pruned to the vendored subset).
static std::vector<std::string> glibc_internal_include_chain(
        const Path& glibc, std::string_view arch) {
    std::string a(arch);
    std::string x86_alt = "x86";  // x86_64 falls back to x86 dirs
    std::vector<std::string> dirs;
    auto add = [&](const std::string& d) { dirs.push_back(d); };

    add((glibc / "include").string());

    // Canonical sysdirs order for arch-linux-gnu (matches the -I chain a
    // real glibc build uses; pruned to the vendored subset).
    auto usl = [&](const std::string& sub) {
        add((glibc / "sysdeps" / "unix" / "sysv" / "linux" / sub).string());
    };
    auto us  = [&](const std::string& sub) {
        add((glibc / "sysdeps" / "unix" / sub).string());
    };
    auto sd  = [&](const std::string& sub) {
        add((glibc / "sysdeps" / sub).string());
    };
    auto sd2 = [&](const std::string& a, const std::string& sub) {
        add((glibc / "sysdeps" / a / sub).string());
    };

    usl(a);
    if (a == "x86_64") usl("x86");
    sd2(a, "nptl");
    if (a == "x86_64") sd2("x86", "nptl");
    usl("wordsize-64");
    usl("include");
    usl("generic");
    usl("");
    sd("nptl");
    sd("pthread");
    sd("gnu");
    us("inet");
    us("sysv");
    us(a);
    if (a == "x86_64") us("x86");
    us("");
    sd("posix");
    sd(a);
    if (a == "x86_64") sd("x86");
    sd("wordsize-64");
    sd("generic");
    add((glibc / "nptl").string());
    // Source root last: internal headers include each other with
    // path-style names like <sysdeps/unix/sysv/linux/sysdep.h>.
    add(glibc.string());
    return dirs;
}

// glibc crt/libc_nonshared inputs: the vendored source subset EXCLUDING
// abilists (stub synthesis reads abilists and must not re-key the crt
// build when it changes).
static ToolchainCacheEntry glibc_objects_entry(const TargetSpec& target,
                                               const Path& glibc) {
    std::vector<std::string> config;
    config.push_back("glibc-objects-v3");
    config.push_back(compiler_identity_block(target));
    config.push_back("glibc:" + std::to_string(target.glibc_major()) +
                     "." + std::to_string(target.glibc_minor()));

    std::vector<Path> inputs;
    for (const char* d : {"csu", "include", "nptl", "stdlib", "io",
                          "debug", "sysdeps"})
        inputs.push_back(glibc / d);
    return toolchain_cache_lookup(target, "glibc-objects", config, inputs);
}

// Stub synthesis inputs: abilists + the target version + a generator
// revision marker.
static ToolchainCacheEntry glibc_stubs_entry(const TargetSpec& target,
                                             const Path& glibc) {
    std::vector<std::string> config;
    config.push_back("glibc-stubs-v4");
    config.push_back("glibc:" + std::to_string(target.glibc_major()) +
                     "." + std::to_string(target.glibc_minor()));
    return toolchain_cache_lookup(target, "glibc-stubs", config,
                                  {glibc / "abilists"});
}


export GlibcObjects ensure_glibc_objects(const TargetSpec& target, LinkMode) {
    GlibcObjects result;
    if (!target.is_linux_gnu()) return result;

    Path lib = find_lib_dir();
    if (lib.string().empty()) return result;

    Path glibc = lib / "libc" / "glibc";
    if (!glibc.is_directory()) {
        std::println(std::cerr,
            "bake: glibc source subset not found at {}\n"
            "  Run scripts/fetch-glibc.sh to vendor glibc for linux-gnu targets.",
            glibc.string());
        return result;
    }

    if (target.glibc_major() < 2 ||
        (target.glibc_major() == 2 && target.glibc_minor() < 28)) {
        std::println(std::cerr,
            "bake: glibc {}.{} is below the vendored baseline (2.28);\n"
            "  use a linux-musl target for older baselines.",
            target.glibc_major(), target.glibc_minor());
        return result;
    }

    std::string arch = target.arch();
    std::string ver = std::to_string(target.glibc_major()) + "." +
                      std::to_string(target.glibc_minor());
    auto entry = glibc_objects_entry(target, glibc);
    Path cache_dir = entry.output_dir;

    result.crt_entry = cache_dir / "Scrt1.o";
    result.libc_nonshared_a = cache_dir / "libc_nonshared.a";
    result.stub_dir = glibc_stubs_entry(target, glibc).output_dir;
    result.dynamic_linker = (arch == "x86_64")
        ? "/lib64/ld-linux-x86-64.so.2"
        : "/lib/ld-linux-aarch64.so.1";

    auto products_ok = [&]() {
        return result.crt_entry.is_regular_file() &&
               result.libc_nonshared_a.is_regular_file();
    };
    if (entry.hit && products_ok()) return result;
    if (products_ok()) {
        toolchain_cache_finish(entry);
        return result;
    }

    std::println("   Compiling glibc crt for {} (glibc {}, cached)",
                 target.triple(), ver);

    // ── Flags (ported from the upstream csu/Makefile build) ──
    std::vector<std::string> base;
    base.push_back(bake_exe_path());
    base.push_back("cc");
    base.push_back("-target");
    base.push_back(target.triple());
    base.push_back("-c");
    base.push_back("-w");
    base.push_back("-fPIC");  // exes may be PIE (clang default) — crt must be PIC
    base.push_back("-O2");
    base.push_back("-DPIC");
    base.push_back("-DMODULE_NAME=libc");
    base.push_back("-DTOP_NAMESPACE=glibc");
    base.push_back("-DNO_INITFINI");
    base.push_back("-Wno-nonportable-include-path");
    base.push_back("-include");
    base.push_back((glibc / "include" / "libc-modules.h").string());
    base.push_back("-include");
    base.push_back((glibc / "include" / "libc-symbols.h").string());

    for (auto& d : glibc_internal_include_chain(glibc, arch))
        base.push_back("-I" + d);
    auto compile_one = [&](const char* src_rel, const char* obj_name,
                           bool is_asm, bool is_c) -> bool {
        Path src = glibc / src_rel;
        Path obj = cache_dir / obj_name;
        std::vector<std::string> cmd = base;
        if (is_asm) {
            cmd.push_back("-DASSEMBLER");
            cmd.push_back("-Wa,--noexecstack");
        }
        if (is_c) {
            cmd.push_back("-std=gnu11");
            cmd.push_back("-fgnu89-inline");
            cmd.push_back("-fmerge-all-constants");
            cmd.push_back("-frounding-math");
            cmd.push_back("-fno-common");
            cmd.push_back("-fmath-errno");
            cmd.push_back("-ftls-model=initial-exec");
            cmd.push_back("-DPIC");
            cmd.push_back("-DLIBC_NONSHARED=1");
        }
        cmd.push_back(src.string());
        cmd.push_back("-o");
        cmd.push_back(obj.string());
        auto r = run_process(cmd, Path(), true);
        if (!r.success()) {
            std::print(std::cerr, "{}", r.stderr_output);
            std::println(std::cerr, "bake: failed to compile {}", src_rel);
            return false;
        }
        return true;
    };

    // ── Scrt1.o = start.S + abi-note.S + init.c, merged relocatable ──
    std::string arch_dir = (arch == "x86_64") ? "x86_64" : "aarch64";
    std::string start_s = "sysdeps/" + arch_dir + "/start.S";
    if (!compile_one(start_s.c_str(), "crt__start.o", true, false))
        return GlibcObjects{};
    // abi-note.S wants csu/ first on its include path.
    {
        std::vector<std::string> cmd = base;
        cmd.push_back("-DASSEMBLER");
        cmd.push_back("-Wa,--noexecstack");
        cmd.push_back("-I" + (glibc / "csu").string());
        cmd.push_back((glibc / "csu" / "abi-note.S").string());
        cmd.push_back("-o");
        cmd.push_back((cache_dir / "crt__abi-note.o").string());
        if (!(cache_dir / "crt__abi-note.o").is_regular_file()) {
            auto r = run_process(cmd, Path(), true);
            if (!r.success()) {
                std::print(std::cerr, "{}", r.stderr_output);
                return GlibcObjects{};
            }
        }
    }
    if (!compile_one("csu/init.c", "crt__init.o", false, true))
        return GlibcObjects{};
    {
        std::vector<std::string> objs = {
            (cache_dir / "crt__start.o").string(),
            (cache_dir / "crt__abi-note.o").string(),
            (cache_dir / "crt__init.o").string(),
            "-o", result.crt_entry.string(),
        };
        std::vector<const char*> argv;
        argv.push_back("ld.lld");
        argv.push_back("-r");
        for (auto& s : objs) argv.push_back(s.c_str());
        if (!result.crt_entry.is_regular_file() &&
            bake_lld_link(LldFlavor::ELF,
                          static_cast<int>(argv.size()),
                          argv.data()) != 0)
            return GlibcObjects{};
    }

    // ── libc_nonshared.a ──
    std::vector<Path> members;
    for (auto* src : glibc_nonshared_sources) {
        std::string stem = src;
        for (auto& c : stem) if (c == '/') c = '_';
        stem = stem.substr(0, stem.rfind('.'));
        Path obj = cache_dir / ("ns__" + stem + ".o");
        if (!compile_one(src, obj.filename().string().c_str(), false, true))
            return GlibcObjects{};
        members.push_back(obj);
    }
    if (!write_archive(result.libc_nonshared_a, members, false))
        return GlibcObjects{};

    toolchain_cache_finish(entry);
    return result;
}

// ── glibc stub libraries — synthesized from the vendored abilists ──
//
// The abilists table (plain text, scripts/glibc-abi-gen.py format) records
// every exported symbol of the eight stub-able glibc libraries with the
// version node at which it was introduced. Link-time stub .so files are
// generated per target glibc version: every symbol whose introduction is
// <= the target version becomes a zero-body definition with a .symver
// directive; the real glibc on the target machine provides implementations
// at run time. This is how one vendored table serves every target version
// from the baseline up to the seed version.

struct GlibcVer {
    int maj = 0, min = 0, pat = 0;
    bool operator<=(const GlibcVer& o) const {
        return std::tie(maj, min, pat) <= std::tie(o.maj, o.min, o.pat);
    }
    bool operator<(const GlibcVer& o) const {
        return std::tie(maj, min, pat) < std::tie(o.maj, o.min, o.pat);
    }
};

struct GlibcAbiTable {
    struct Lib {
        std::string name;
        int sover = 0;
        bool removed = false;
        GlibcVer removed_in{};
    };
    struct Sym {
        std::string name;
        int lib = 0;
        bool is_obj = false;
        long size = 0;
        std::vector<GlibcVer> vers;
    };
    std::vector<Lib> libs;
    std::vector<std::string> targets;          // declaration order
    std::map<std::string, std::vector<Sym>> syms;
};

static bool glibc_parse_version(std::string_view s, GlibcVer& v) {
    int parts[3] = {0, 0, 0};
    int idx = 0;
    std::size_t i = 0;
    while (i < s.size() && idx < 3) {
        int val = 0;
        bool any = false;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            val = val * 10 + (s[i] - '0');
            ++i;
            any = true;
        }
        if (!any) return false;
        parts[idx++] = val;
        if (i < s.size()) {
            if (s[i] != '.') return false;
            ++i;
        }
    }
    if (idx == 0 || i != s.size()) return false;
    v = {parts[0], parts[1], parts[2]};
    return true;
}

static bool glibc_parse_abilists(std::string_view text, GlibcAbiTable& t) {
    std::string current_target;
    std::size_t pos = 0;
    auto next_line = [&](std::string_view& line) {
        if (pos >= text.size()) return false;
        std::size_t eol = text.find('\n', pos);
        if (eol == std::string_view::npos) eol = text.size();
        line = text.substr(pos, eol - pos);
        pos = eol + 1;
        return true;
    };
    auto split_ws = [](std::string_view s) {
        std::vector<std::string_view> out;
        std::size_t i = 0;
        while (i < s.size()) {
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
            std::size_t start = i;
            while (i < s.size() && s[i] != ' ' && s[i] != '\t') ++i;
            if (i > start) out.push_back(s.substr(start, i - start));
        }
        return out;
    };
    auto parse_vers = [&](std::string_view csv,
                          std::vector<GlibcVer>& out) -> bool {
        std::size_t i = 0;
        while (i <= csv.size()) {
            std::size_t comma = csv.find(',', i);
            if (comma == std::string_view::npos) comma = csv.size();
            GlibcVer v;
            if (!glibc_parse_version(csv.substr(i, comma - i), v))
                return false;
            out.push_back(v);
            if (comma == csv.size()) break;
            i = comma + 1;
        }
        return !out.empty();
    };

    std::string_view line;
    while (next_line(line)) {
        if (line.empty() || line[0] == '#') continue;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (!line.empty() && line[0] == '[') {           // [target X]
            std::size_t rb = line.find(']');
            if (rb == std::string_view::npos) return false;
            current_target = std::string(line.substr(8, rb - 8));
            t.syms[current_target];
            continue;
        }
        auto f = split_ws(line);
        if (f.empty()) continue;
        if (f[0] == "format" || f[0] == "seed") continue;
        if (f[0] == "lib" && f.size() >= 4) {
            GlibcAbiTable::Lib l;
            l.name = std::string(f[1]);
            l.sover = std::stoi(std::string(f[3]));
            if (f.size() >= 6 && f[4] == "removed-in")
                if (glibc_parse_version(f[5], l.removed_in))
                    l.removed = true;
            t.libs.push_back(std::move(l));
            continue;
        }
        if (f[0] == "target" && f.size() == 2) {
            t.targets.emplace_back(f[1]);
            continue;
        }
        if (current_target.empty()) continue;
        if (f[0] == "fn" && f.size() == 4) {
            GlibcAbiTable::Sym s;
            s.name = std::string(f[1]);
            for (std::size_t i = 0; i < t.libs.size(); ++i)
                if (t.libs[i].name == f[2]) { s.lib = static_cast<int>(i); break; }
            if (!parse_vers(f[3], s.vers)) return false;
            t.syms[current_target].push_back(std::move(s));
            continue;
        }
        if (f[0] == "obj" && f.size() == 5) {
            GlibcAbiTable::Sym s;
            s.name = std::string(f[1]);
            s.is_obj = true;
            s.size = std::stol(std::string(f[3]));
            for (std::size_t i = 0; i < t.libs.size(); ++i)
                if (t.libs[i].name == f[2]) { s.lib = static_cast<int>(i); break; }
            if (!parse_vers(f[4], s.vers)) return false;
            t.syms[current_target].push_back(std::move(s));
            continue;
        }
    }
    return !t.libs.empty() && !t.syms.empty();
}


export Path ensure_glibc_stubs(const TargetSpec& target) {
    if (!target.is_linux_gnu()) return Path();

    Path lib = find_lib_dir();
    if (lib.string().empty()) return Path();

    Path abilists = lib / "libc" / "glibc" / "abilists";
    auto content = read_file(abilists);
    if (!content) {
        std::println(std::cerr,
            "bake: glibc abilists not found at {}\n"
            "  Run scripts/fetch-glibc.sh to vendor glibc.",
            abilists.string());
        return Path();
    }

    GlibcAbiTable table;
    if (!glibc_parse_abilists(*content, table)) {
        std::println(std::cerr, "bake: malformed glibc abilists ({})",
                     abilists.string());
        return Path();
    }

    std::string triple = target.triple();
    if (!table.syms.count(triple)) {
        std::println(std::cerr,
            "bake: glibc abilists has no data for {} (supported: {})",
            triple, [&]{
                std::string j;
                for (auto& [k, _] : table.syms) {
                    if (!j.empty()) j += ", ";
                    j += k;
                }
                return j;
            }());
        return Path();
    }

    GlibcVer target_ver{target.glibc_major(), target.glibc_minor(), 0};
    Path glibc = find_lib_dir() / "libc" / "glibc";
    auto entry = glibc_stubs_entry(target, glibc);
    Path stub_dir = entry.output_dir;

    bool complete = entry.hit;
    if (complete) {
        for (auto& l : table.libs) {
            if (l.removed && !(target_ver < l.removed_in)) continue;
            Path so = stub_dir / ("lib" + l.name + ".so." +
                                  std::to_string(l.sover));
            if (!so.is_regular_file()) { complete = false; break; }
        }
    }
    if (complete) return stub_dir;
    // Products exist but the manifest was never written (interrupted
    // build): finish it instead of resynthesizing.
    {
        bool all = true;
        for (auto& l : table.libs) {
            if (l.removed && !(target_ver < l.removed_in)) continue;
            if (!(stub_dir / ("lib" + l.name + ".so." +
                              std::to_string(l.sover))).is_regular_file()) {
                all = false;
                break;
            }
        }
        if (all) {
            toolchain_cache_finish(entry);
            return stub_dir;
        }
    }

    stub_dir.mkdir_recursive();
    std::println("   Synthesizing glibc stubs for {} (glibc {}.{}), cached",
                 triple, target_ver.maj, target_ver.min);

    bool ptr64 = target.arch() == "x86_64" || target.arch() == "aarch64";
    std::string word = ptr64 ? ".quad" : ".long";
    int psize = ptr64 ? 8 : 4;
    std::string ld_soname = (target.arch() == "x86_64")
        ? "ld-linux-x86-64.so.2" : "ld-linux-aarch64.so.1";

    for (auto& l : table.libs) {
        if (l.removed && !(target_ver < l.removed_in)) continue;

        std::string asm_text = ".text\n";
        std::vector<GlibcVer> used_vers;

        for (auto& s : table.syms[triple]) {
            if (s.lib >= static_cast<int>(table.libs.size())) continue;
            if (table.libs[s.lib].name != l.name) continue;

            // Versions eligible at this target; default = greatest.
            std::vector<const GlibcVer*> elig;
            for (auto& v : s.vers)
                if (v <= target_ver) elig.push_back(&v);
            if (elig.empty()) continue;
            const GlibcVer* def = elig[0];
            for (auto* v : elig) if (*def < *v) def = v;

            for (auto* vp : elig) {
                auto& v = *vp;
                std::string mangled = s.name + "_" +
                    std::to_string(v.maj) + "_" + std::to_string(v.min);
                if (v.pat != 0)
                    mangled += "_" + std::to_string(v.pat);
                std::string at = (vp == def) ? "@@" : "@";
                asm_text += ".balign " + std::to_string(psize) + "\n";
                asm_text += ".globl " + mangled + "\n";
                if (s.is_obj) {
                    asm_text += ".type " + mangled + ", %object\n";
                    asm_text += ".size " + mangled + ", " +
                                std::to_string(s.size) + "\n";
                } else {
                    asm_text += ".type " + mangled + ", %function\n";
                }
                asm_text += ".symver " + mangled + ", " + s.name + at +
                            "GLIBC_" + std::to_string(v.maj) + "." +
                            std::to_string(v.min);
                if (v.pat != 0)
                    asm_text += "." + std::to_string(v.pat);
                asm_text += ", remove\n";
                if (s.is_obj)
                    asm_text += mangled + ": .fill " +
                                std::to_string(s.size) + ", 1, 0\n";
                else
                    asm_text += mangled + ": " + word + " 0\n";
                bool known = false;
                for (auto& u : used_vers) if (!(u < v) && !(v < u)) known = true;
                if (!known) used_vers.push_back(v);
            }
        }

        // A lib with no eligible symbols at this target version (empty
        // stubs, missing ld.so in the seed build) gets no stub at all —
        // prepare_runtime only links stubs that exist on disk.
        if (used_vers.empty()) continue;

        // libc only: carry a weak reference to _IO_stdin_used, exactly as
        // the real libc.so.6 does. The executable always defines the symbol
        // (csu/init.c inside Scrt1.o); a reference from a linked DSO makes
        // the linker export that definition into the executable's dynamic
        // symbol table, where glibc's runtime probe looks for it (its
        // absence marks a pre-2.1 program and selects the legacy FILE
        // compatibility path). The pointer must live in .data.rel.ro: a
        // data relocation against a preemptible symbol is only valid in a
        // writable section, and .rodata is not one.
        if (l.name == "c") {
            asm_text += ".weak _IO_stdin_used\n";
            asm_text += ".section .data.rel.ro\n";
            asm_text += ".balign " + std::to_string(psize) + "\n";
            asm_text += word + " _IO_stdin_used\n";
        }

        asm_text += ".data\n";

        // Version script: empty nodes for every version we reference.
        std::string map_text;
        std::sort(used_vers.begin(), used_vers.end(),
                  [](auto& a, auto& b) { return a < b; });
        for (auto& v : used_vers) {
            map_text += "GLIBC_" + std::to_string(v.maj) + "." +
                        std::to_string(v.min);
            if (v.pat != 0) map_text += "." + std::to_string(v.pat);
            map_text += " { };\n";
        }

        std::string stem = "lib" + l.name + ".so." + std::to_string(l.sover);
        Path asm_file = stub_dir / (stem + ".s");
        Path map_file = stub_dir / (stem + ".map");
        Path obj_file = stub_dir / (stem + ".o");
        Path out_file = stub_dir / stem;
        write_file(asm_file, asm_text);
        write_file(map_file, map_text);

        std::vector<std::string> cc = {bake_exe_path(), "cc",
            "-target", triple, "-c", "-w", asm_file.string(),
            "-o", obj_file.string()};
        auto r = run_process(cc, Path(), true);
        if (!r.success()) {
            std::print(std::cerr, "{}", r.stderr_output);
            std::println(std::cerr, "bake: failed to assemble {} stub",
                         l.name);
            return Path();
        }

        std::string soname = (l.name == "ld") ? ld_soname : stem;
        // Keep the strings alive: c_str() on temporaries would dangle
        // before bake_lld_link consumes argv.
        std::string obj_path = obj_file.string();
        std::string map_path = map_file.string();
        std::string out_path = out_file.string();
        std::vector<const char*> argv;
        argv.push_back("ld.lld");
        argv.push_back("-shared");
        argv.push_back(obj_path.c_str());
        argv.push_back("-soname");
        argv.push_back(soname.c_str());
        argv.push_back("-version-script");
        argv.push_back(map_path.c_str());
        argv.push_back("-o");
        argv.push_back(out_path.c_str());
        if (bake_lld_link(LldFlavor::ELF,
                          static_cast<int>(argv.size()),
                          argv.data()) != 0) {
            std::println(std::cerr, "bake: failed to link {} stub", stem);
            return Path();
        }
    }

    toolchain_cache_finish(entry);
    return stub_dir;
}
// ── Unified runtime preparation ──
//
// Called once per link job from bake_clang_driver.cpp. Dispatches to the
// family-specific ensure_* functions and assembles a RuntimeArtifacts
// struct. This is the ONLY function the driver needs to call — all
// target-specific decisions are encapsulated here.

export RuntimeArtifacts prepare_runtime(
        const TargetSpec& target, bool is_cxx, LinkMode link_mode) {
    RuntimeArtifacts rt;
    LibcFamily family = resolve_libc_family(target);

    // compiler-rt: all targets
    rt.compiler_rt = ensure_compiler_rt_objects(target);

    // C++ runtime: all targets when is_cxx
    if (is_cxx) {
        auto cxx = ensure_cxx_runtime(target);
        rt.libcxx    = cxx.libcxx_a;
        rt.libcxxabi = cxx.libcxxabi_a;
        rt.libunwind = cxx.libunwind_a;
    }

    switch (family) {
    case LibcFamily::Musl: {
        auto musl = ensure_musl_objects(target);
        rt.libc = musl.libc_a;
        switch (link_mode) {
        case LinkMode::Static:     rt.crt_entry = musl.crt1_o;  break;
        case LinkMode::StaticPie:  rt.crt_entry = musl.rcrt1_o; break;
        case LinkMode::Dynamic:    rt.crt_entry = musl.Scrt1_o; break;
        }
        break;
    }
    case LibcFamily::Darwin: {
        // Deployment minimum from the target spec (explicit -target
        // suffix or the built-in default) — the target query is
        // authoritative for compile and link alike.
        rt.macos_deployment_target = target.macos_deployment_min();
        // libSystem directory, frameworks, and SDK version: system SDK
        // when native (xcrun), vendored stubs when cross — one place
        // decides (shared with the sanitizer dylib linker).
        auto face = resolve_darwin_link_face(target);
        rt.macos_sdk_version = face.sdk_version;
        rt.link_dirs.push_back(face.lib_dir);
        if (!face.framework_dir.empty())
            rt.framework_dirs.push_back(face.framework_dir);
        break;
    }
    case LibcFamily::Windows: {
        auto mingw = ensure_mingw_objects(target);
        rt.libc = mingw.libmingw32_a;
        // Don't inject crt entry — the Clang driver adds crt2.o itself as
        // a bare filename. We just need it in the -L search path (below).
        // Link search paths: cache dir (for crt2.o, libmingw32.a) + import libs.
        if (!mingw.crt2_o.string().empty()) {
            rt.link_dirs.push_back(mingw.crt2_o.parent().string());
            rt.link_dirs.push_back(mingw.import_lib_dir.string());
            rt.mingw_import_dir = mingw.import_lib_dir.string();
        }
        // Always-link system libraries.
        for (auto* lib : mingw_always_link_libs)
            rt.always_link_libs.push_back(lib);
        break;
    }
    case LibcFamily::Gnu: {
        // gnu: dynamic-only. Static/static-pie on glibc is rejected with
        // guidance toward musl (bakeExecuteJob checks before dispatch).
        if (link_mode != LinkMode::Dynamic) {
            std::println(std::cerr,
                "bake: static linking is not supported for glibc targets\n"
                "  (glibc's static mode has broken dlopen/NSS semantics);\n"
                "  use a linux-musl target for static builds.");
            break;
        }
        // Native gnu keeps the vendored crt + synthesized stubs (they link
        // against libc.so.6 by soname; the runtime libc is the system one),
        // but unknown -l<name> must resolve against the system lib dirs.
        if (resolve_gnu_sdk(target) == GnuSdkLayout::SystemGnu) {
            std::string multi = target.arch() == "x86_64"
                ? "/usr/lib/x86_64-linux-gnu" : "/usr/lib/aarch64-linux-gnu";
            rt.link_dirs.push_back(multi);
            rt.link_dirs.push_back("/usr/lib64");
            rt.link_dirs.push_back("/usr/lib");
        }
        auto gnu = ensure_glibc_objects(target, link_mode);
        rt.crt_entry = gnu.crt_entry;
        rt.libc = gnu.libc_nonshared_a;
        rt.dynamic_linker = gnu.dynamic_linker;
        Path stubs = ensure_glibc_stubs(target);
        if (!stubs.string().empty()) {
            rt.link_dirs.push_back(stubs.string());
            // Fixed set, link order; merged-into-libc libs stop at 2.34.
            static const struct {
                const char* name; int sover; const char* removed_in;
            } gnu_libs[] = {
                {"m", 6, nullptr}, {"c", 6, nullptr},
                {"ld", 2, nullptr}, {"resolv", 2, nullptr},
                {"pthread", 0, "2.34"}, {"dl", 2, "2.34"},
                {"rt", 1, "2.34"}, {"util", 1, "2.34"},
            };
            for (auto& gl : gnu_libs) {
                if (gl.removed_in) {
                    GlibcVer floor;
                    glibc_parse_version(gl.removed_in, floor);
                    GlibcVer tv{target.glibc_major(),
                                target.glibc_minor(), 0};
                    if (!(tv < floor)) continue;
                }
                Path so = stubs / ("lib" + std::string(gl.name) + ".so." +
                                   std::to_string(gl.sover));
                if (so.is_regular_file())
                    rt.gnu_stub_libs.push_back(so.string());
            }
        }
        break;
    }
    case LibcFamily::None:
        break;
    }

    return rt;
}

} // namespace bake
