# AGENTS.md — bake

## What is bake

bake is an all-in-one C/C++ build system and compiler toolchain. Think "Cargo for C++" meets `zig cc`: default source discovery, programmable `build.cpp` input declarations, package management, and an integrated LLVM/Clang toolchain — all in one binary, zero external dependencies.

APIs are not frozen — make changes clean, not backward-compatible.

## Iteration Posture

Fast iteration, quality-first. When these rules conflict with minimal-diff
instincts, these rules win:

- **Growth justifies breaking changes.** Requirements start small and grow.
  When one outgrows its shape — signature, type, module boundary — reshape it
  and migrate every caller in the same change. Never add a parallel path,
  adapter, compat flag, or defaulted parameter to dodge a break.
- **Refactoring is in scope.** If a change makes neighboring code wrong or
  redundant, fixing or deleting those neighbors belongs to the task. Never
  leave old and new paths both alive; cutover clean.
- **Map before writing.** Before designing, find how the concern is currently
  handled everywhere — search concept keywords, not one file. Before adding
  any function/type/constant, search for an existing equivalent. Two similar
  implementations → unify; never add a third.
- **Reshape instead of stacking.** If a change fights the current structure,
  the structure is wrong — change the structure. Wrappers, pre/post passes,
  and special cases layered on top are rejected; inline them or reshape what
  is underneath.

## Build & Test

```bash
# Stage 0: bootstrap with system Clang (requires CMake ≥ 3.30, Ninja, Clang ≥ 22 + libc++)
cmake -G Ninja -B build && cmake --build build

# Run the test suite (e2e + graph + parser units)
ctest --test-dir build --output-on-failure

# Self-host needs the two bake-pkgs packages fetched first (json, tomlplusplus)
./build/bake update

# Self-host: stage 0 bake builds itself → out/bin/bake
./build/bake build

# Clean self-hosted output
rm -rf out/
```

## Cross-Platform Compilation

bake cross-compiles to four libc families, all from any host:

| Target triple | libc | Arch |
|---|---|---|
| `*-darwin` | host libSystem (macOS) | aarch64, x86_64 |
| `*-linux-musl` | musl (vendored source) | aarch64, x86_64 |
| `*-linux-gnu` | glibc (vendored subset + synthesized stubs) | x86_64, aarch64 |
| `*-windows-gnu` | MinGW-w64 (vendored source) | x86_64, aarch64 |

Use `-t <triple>` to cross-compile (e.g. `bake build -t x86_64-linux-gnu`).
glibc targets default to version 2.28; pick another with a triple suffix
(`-t x86_64-linux-gnu.2.36`). The suffix governs both the link surface
(per-version stub libraries) and the header surface (`__GLIBC__`/
`__GLIBC_MINOR__` pinned to the target — headers are vendored from the
newest glibc). gnu is dynamic-only — static builds are rejected with musl
guidance.

The dispatch chain in `bake.toolchain.runtime`:

1. `resolve_libc_family(target)` → `LibcFamily` (`Darwin`, `Musl`, `Windows`, `Gnu`, `None`)
2. `prepare_runtime(target)` → calls the right `ensure_*_objects()` builder
3. `make_compile_command()` / `make_link_command()` (`bake.buildsystem.cmdgen`) → target-specific flags
4. LLD flavor selected in `driver.cppm` (`Darwin`, `COFF/MinGW`, `GNU`)

Each libc family is self-contained:
- **Darwin**: SDK stubs (`libSystem.tbd`) shipped in `lib/libc/darwin/`. No Xcode needed.
- **Musl**: full musl source in `lib/libc/musl/`, built from source → static CRT.
- **Gnu**: crt + `libc_nonshared.a` compiled in-process from the vendored glibc 2.28 source subset (`lib/libc/glibc/`); libc itself is never built — link-time stub `.so` files are synthesized per target version from the vendored `abilists` symbol/version table (`ensure_glibc_stubs`). Headers are the newest glibc's install-headers (`lib/libc/include/generic-glibc/` + per-triple dirs), with `features.h` patched so `-D__GLIBC__`/`-D__GLIBC_MINOR__` pin the target version. `scripts/fetch-glibc.sh` regenerates everything from upstream tarballs (patches in `scripts/glibc-patches/`).
- **Windows**: MinGW-w64 v14 in `lib/libc/mingw/`, CRT + winpthreads from source. Import libraries generated on-demand from `.def` files (only libraries referenced by `-l` flags). LLD MinGW driver. UCRT default via `_mingw.h`.

libc++ config site varies per target (`lib/libcxx/cross-config/` for musl, `lib/libcxx/gnu-config/` for gnu, `lib/libcxx/mingw-config/` for windows).

compiler-rt `chkstk.S` patched with `__chkstk`/`_alloca`/`__alloca` aliases — normally provided by `libgcc.a`, bake provides them since it doesn't use GCC runtime.

## Design

### Moid

A "moid" (模具) is bake's unit of compilation — one `bake.toml` describes one moid.

| Type | Produces | Consumers get |
|---|---|---|
| `executable` (default) | Binary in `out/bin/` | — |
| `lib` | Static archive in `out/lib/` + module PCMs | Public headers, module interfaces, linked objects |
| `dylib` | Shared library in `out/lib/` | Public headers, module interfaces |

### Build pipeline (single path)

Every moid follows one configure and build pipeline:

1. **resolve_moid_graph** — resolve workspace + dependencies into a topological `MoidGraph`.
2. **configure_moid_graph** — for each moid: obtain inputs from default discovery or `build.cpp`, merge authoritative `bake.toml` configuration, then write one `MoidDeclaration` JSON in `out/.bake/`.
3. **build_graph** — translate declarations into a flat `BuildAction` list (compile, compile_module, archive, link) with dependency edges.
4. **execute_graph** — parallel executor with content-based fingerprinting. Dirty detection + propagation, thread pool, incremental rebuilds.

### Integrated compiler toolchain

bake embeds LLVM + Clang + LLD as linked libraries. No subprocesses:

- **Compilation**: `driver.cppm` shims the Clang driver; `cc1.cppm` / `cc1as.cppm` are the in-process cc1/assembler entries (stock Clang sources).
- **Linking**: `bake.toolchain.lld` (`toolchain/lld.cppm`) invokes LLD in-process (Darwin, MinGW, GNU flavors).
- **Module scanning**: text-based in-process scanner in `bake.engine`. No `clang-scan-deps`.
- **`import std`**: `ensure_std_modules()` pre-builds `std.pcm` from vendored libc++ sources, cached in `~/.cache/bake/<key>/std/`.
- **Static libc++**: `ensure_libcxx_objects()` compiles libc++ + libc++abi + libunwind from vendored sources, cached in `~/.cache/bake/libcxx-objects/`.

### Workspace

Multi-package like Cargo. Each member has its own `bake.toml`. Root declares `[workspace] members = [...]`. Workspace-internal deps use path deps and bypass the package cache.

### `bake.build` module

`lib/bake/bake.build.cppm` is the `build.cpp` API — source-distributed (like a header), compiled fresh per project when `build.cpp` is present. Context arrives via environment variables (`BAKE_MOID_ID`, `BAKE_SOURCE_DIR`, `BAKE_TARGET`, `BAKE_DEPS`, …). The `Builder` describes **inputs** (which source files, public headers, prebuilt libs, extra binaries, test registrations). It does NOT set flags, defines, or system libraries — those are in `bake.toml` (`[profile.*]`, `[target.*]`, `[link]`, `[options]`). The script writes a declaration JSON to `BAKE_DECLARATION_PATH`.

## Directory Layout

```
bake/
├── bake.toml               [workspace] members = ["bake"]
├── CMakeLists.txt          Stage 0 bootstrap (permanent)
├── bootstrap/
│   └── build                cross-compile bake to any target
├── bake/                   the bake executable package
│   ├── bake.toml
│   ├── build.cpp           self-hosting build script
│   └── src/
│       ├── main.cpp             composition root: multicall dispatch + build commands
│       ├── util.cppm            Path, SHA-256, glob, process spawn, bake_exe_path
│       ├── json.cppm            bake.json interface (DOM, accessors)
│       ├── toml.cppm            bake.toml interface (Table/Node model)
│       ├── json.cpp             bake.json impl: nlohmann/json (self-host, via bake-pkgs)
│       ├── toml.cpp             bake.toml impl: toml++ (self-host, via bake-pkgs)
│       ├── stage0/              hand-written bake.json/bake.toml impls (CMake bootstrap)
│       ├── toolchain/           bake.toolchain.* — the embedded toolchain (mechanism)
│       │   ├── target.cppm                TargetSpec, triple parsing/matching
│       │   ├── lld.cppm                   in-process LLD link + archive write
│       │   ├── runtime.cppm               ensure_* runtime builders, toolchain cache
│       │   ├── cc1.cppm                   cc1 frontend (stock Clang cc1_main.cpp)
│       │   ├── cc1as.cppm                 integrated assembler (stock cc1as_main.cpp)
│       │   └── driver.cppm                Clang driver shim (bake cc / bake c++)
│       └── buildsystem/         bake.buildsystem.* — the build system (policy)
│           ├── project.cppm               Manifest, Lockfile, layout
│           ├── cmdgen.cppm                build intent → compiler argv/flags/macros
│           ├── moid.cppm                  MoidDeclaration model, JSON codec
│           ├── graph.cppm                 MoidGraph, dependency resolution
│           ├── engine.cppm                configure, source discovery, DAG, executor
│           └── package.cppm               Resolver, Fetcher, Cache, lockfile
├── lib/                   vendored runtime (distributed with bake)
│   ├── bake/                    bake.build.cppm (build.cpp API)
│   ├── libcxx/                  libc++ (include/, src/, modules/)
│   ├── libcxxabi/               libc++abi
│   ├── libunwind/               libunwind
│   ├── compiler-rt/             compiler-rt (builtins)
│   ├── libc/
│   │   ├── darwin/              macOS SDK stubs
│   │   ├── musl/                musl source
│   │   ├── glibc/               glibc source subset + abilists (linux-gnu)
│   │   ├── mingw/               MinGW-w64 headers + CRT sources
│   │   └── include/             multi-platform headers (linux, generic)
│   └── include/                 vendored Clang/LLVM headers
├── scripts/
│   ├── build-llvm.sh            build LLVM into external/
│   ├── fetch-darwin-headers.sh  extract macOS SDK stubs
│   ├── fetch-mingw.sh           download MinGW-w64 v14 from GitHub
│   ├── fetch-musl.sh            download musl source
│   ├── fetch-linux-headers.sh   kernel UAPI headers (shared musl+gnu; scsi.h/sg.h from libc6-dev)
│   ├── fetch-glibc.sh           vendor glibc subset + abilists from upstream
│   ├── glibc-abi-gen.py         tarball .abilist files → abilists (host-side)
│   └── update-runtime.sh        update vendored libc++/libcxxabi/libunwind/compiler-rt
├── tests/
│   ├── test_runner.cpp          custom test framework, spawns bake binary
│   ├── graph_test.cpp           unit tests for graph/moid logic
│   ├── parser_test.cpp          contract tests for bake.json / bake.toml
│   └── projects/                fixture projects (64 test cases)
└── external/              LLVM source + prebuilt install (git submodule)
```

## Manifest Format

```toml
[package]
name = "myapp"
version = "0.1.0"
type = "executable"          # executable (default) | lib | dylib

[language]                    # defaults: c++17 / c17
cxx = "c++23"                # c++17 | c++20 | c++23
c = "c17"                    # c11 | c17 | c23

[options]                    # bool-only feature flags → BAKE_<MOID>_<OPTION> macros
use_tls = false
use_json = true

[dependencies]
somelib = { path = "../somelib" }
remotepkg = { url = "https://github.com/org/repo", tag = "v1.0" }
curl = { path = "../curl", options = ["use_tls"] }   # activate features

[link]                       # platform-agnostic system libraries
libraries = ["z"]

[target."*-apple-darwin"]    # triple-glob matching: * matches a full segment
frameworks = ["Foundation"]

[target."*-linux-musl"]
libraries = ["pthread", "dl"]

[profile.release]            # override built-in release defaults
opt = 3
lto = true
strip = true

[sources]                    # file extension config (all have defaults)
# module_ext = [".cppm", ".ixx"]
# source_ext = [".cpp", ".cc", ".cxx", ".c"]
# header_ext = [".h", ".hpp", ".hxx", ".hh"]
```

Profiles (`--release` / `--profile <name>`, default `dev`) control compiler flags via semantic fields (`opt`, `debug`, `lto`, `strip`, `sanitize`, `warnings`). Options are bool-only — OR-merged across the dependency graph. Auto-macros: `BAKE_<MOID>_<OPTION>=0/1`, `BAKE_<MOID>_VERSION="..."`.

## Output Directory

```
out/
├── <target-triple>/          # e.g. aarch64-apple-darwin
│   ├── bin/                  executables
│   ├── lib/                  .a / .dylib / .so / .dll
│   ├── .obj/                 object files
│   ├── .bmi/                 module PCMs
│   └── .bake/                declarations, fingerprints, graph.json
└── .bake/                    host-level (build.cpp compiled scripts)

## Global Toolchain Cache

Content-addressed, one directory per target triple (with version suffixes:
`x86_64-linux-gnu.2.36` is separate from `x86_64-linux-gnu`):

```
~/.cache/bake/
├── .identity/                  compiler identity blocks (perf cache)
├── .gen/                       generated std module sources
└── <triple>/
    ├── h/<config-hash>.txt     manifest: final digest + per-input-file
    │                           size/mtime/digest lines (mtime hit = no rehash)
    └── o/<final-hash>/         products: std.pcm, libc++.a, libc.a, stubs,
                                crt, bake.build.pcm, sanitizer runtimes…
```

Config hash inputs: compiler identity + target surface lines (glibc gate
version, darwin deployment min) + the unit list. File digests cover the
vendored sources/headers — any vendor edit re-keys automatically; there are
no manual bump markers. Managed by `toolchain_cache_lookup`/`finish`.

## Sanitizers

- Supported: `-fsanitize=address` and `-fsanitize=undefined` — runtimes
  built from the vendored compiler-rt sources
  (`lib/compiler-rt/lib/{sanitizer_common,interception,ubsan,asan,lsan}`)
  at first link, cached like any runtime product. Each platform uses its
  official form: ELF (linux-gnu/musl) static archives (asan
  whole-archive'd; embeds lsan_common, the ubsan reporting core and
  operator new/delete), darwin shared dylibs (`@rpath` install names, the
  driver injects the cache-dir rpath — the official Clang layout; cxxabi
  and operator-new references stay undefined like upstream's dylibs),
  windows-gnu ubsan static archive + asan shared DLL (x86_64 only, per
  upstream; the DLL is copied next to the executable).
- Runtime compile flags include `-fno-builtin` (loop-idiom recognition
  would otherwise route internal_* loops through the interceptors and
  deadlock init); every flag is part of the cache config key.
- Sanitizers are a native development tool: native builds are the
  contract; cross-built sanitized products are not validated.
- Other sanitizers (tsan, msan, ...) are not vendored: compile-only
  invocations still instrument; the link is rejected with a clear
  message.

## Coding Style

- **C++23**, strict standard (`CMAKE_CXX_EXTENSIONS OFF`), Clang + libc++.
- **`import std;`** — all standard library access via `import std;`, not `#include`. The only `#include` in module files are platform/OS headers (`<errno.h>`, `<sys/wait.h>`, `<windows.h>`) in the **global module fragment** (`module;` before `export module`).
- **I/O**: `std::println(...)` for stdout, `std::println(std::cerr, ...)` for stderr. `{}` format placeholders, not printf. No `fprintf`/`printf`/bare `stderr`/`stdout`.
- **Module naming**: `bake.<scope>.<name>` with two scopes — `bake.toolchain.*` (the embedded toolchain: mechanism, must never import `bake.buildsystem.*`) and `bake.buildsystem.*` (build system: policy, may import `bake.toolchain.*`). Module files carry only their leaf name; the scope comes from the directory (`toolchain/target.cppm` → `bake.toolchain.target`, `util.cppm` at src/ root → `bake.util`). `bake.build.cppm` (downstream build.cpp API, in lib/bake/) keeps its full name.
- **Module chain**: { `bake.json`, `bake.toml` } (two impls per module: hand-written `src/stage0/*.cpp` for the CMake bootstrap, nlohmann/json + toml++ via bake-pkgs for the self-host) → `bake.util` → { `bake.net`, `bake.archive` } (two impls per module: stage0 spawns system curl/tar/unzip for the CMake bootstrap, libcurl + libarchive via bake-pkgs for the self-host) → { `bake.toolchain.target`, `bake.buildsystem.project` } → { `bake.toolchain.lld`, `bake.buildsystem.cmdgen` } → `bake.toolchain.runtime` → { `bake.toolchain.cc1`, `bake.toolchain.cc1as` } → `bake.toolchain.driver` → { `bake.buildsystem.moid` / `graph`, `engine` / `package` } → `main.cpp` (composition root, no CLI module). The two scopes meet only at `TargetSpec` + plain argv strings.
- **User-facing output uses `macos`** not `darwin` for platform names.
- **Commit messages**: conventional commits, subject-only ≤ 50 chars. Describe what changed, not why.

## Key Constraints

- **Do NOT remove `CMakeLists.txt`** — permanent Stage 0 bootstrap.
- **No external toolchain dependencies** — bake embeds LLVM/Clang/LLD, vendors libc++/libc++abi/libunwind/compiler-rt/musl/MinGW-w64. System Clang is only for Stage 0 bootstrap.
- **`bake build` never moves a locked tag** — only `bake update` re-resolves tags to commits.
- **`--locked`** fails if lock is missing/stale; **`--offline`** bans network; **`--frozen`** = both.
- **Static by default** — link actions prefer static archives over shared libraries.
- **Keep `docs/` in sync** — any content or API change must be reflected in the
  bilingual usage book under `docs/`.
