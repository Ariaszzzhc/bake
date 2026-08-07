# AGENTS.md — bake

## What is bake

bake is an all-in-one C/C++ build system and compiler toolchain, written in C++23. Think "Cargo for C++" meets "zig cc": convention-based builds with a `build.cpp` escape hatch, package management, and an integrated LLVM/Clang toolchain so it has zero external dependencies.

The project is pre-release. APIs are not frozen — make changes clean, not backward-compatible.

## Build & Test

```bash
# Stage 0 bootstrap (CMake + Ninja + Clang with libc++)
cmake -G Ninja -B build && cmake --build build

# Run the test suite (55 end-to-end tests)
ctest --test-dir build --output-on-failure

# Run a single test by name filter
./build/test_runner ./build/bake "workspace"

# Self-host: stage 0 bake builds itself → out/
./build/bake build

# Clean bake's own output
rm -rf out/
```

Requires: CMake ≥ 3.30, Ninja, Clang ≥ 19 with libc++ (Homebrew `llvm` formula on macOS).

Stage 0 uses the system Clang. The self-hosted build (stage 1) uses bake's own integrated compiler and vendored libc++.

## Language & Module Conventions

- **C++23**, strict standard (`CMAKE_CXX_EXTENSIONS OFF`), Clang + libc++.
- **`import std;`** — all standard library access is via `import std;`, not `#include`. The only `#include` directives in module files are for platform/OS headers (`<errno.h>`, `<sys/wait.h>`, `<windows.h>`), which go in the **global module fragment** (`module;` before `export module`).
- **I/O**: use `std::println(...)` for stdout, `std::println(std::cerr, ...)` for stderr, `std::print(...)` without trailing newline. Use `{}` format placeholders, not printf `%s`. Drop `.c_str()` — `std::format` handles `std::string` natively. Do NOT use `fprintf`, `printf`, or the bare `stderr`/`stdout` macros.
- **Module naming**: `bake.<subsystem>` (e.g. `bake.util`, `bake.engine`). File name mirrors module name: `bake.<name>.cppm`.
- **Module dependency chain**: `bake.util` → `bake.project` → `bake.compiler` → `bake.moid` / `bake.graph` → `bake.engine` / `bake.package` → `bake.cli`. Third-party modules (`nlohmann.json`, `tomlplusplus`) are consumed as `lib`-type moid packages.

## Directory Layout

```
bake-compiler/            workspace root
├── bake.toml             [workspace] members = ["bake"]
├── CMakeLists.txt        Stage 0 bootstrap (permanent, not temporary)
├── bake/                 the bake executable package
│   ├── bake.toml         moid manifest (type = "executable")
│   ├── build.cpp         build.cpp for bake itself (self-hosting)
│   └── src/
│       ├── main.cpp           import bake.cli; → bake::cli::main()
│       ├── cli.cppm           CLI dispatch, all commands (init/build/add/update/run/clean/test)
│       ├── bake.util.cppm     Path, SHA-256, glob, process spawn
│       ├── bake.project.cppm  bake.toml model, Manifest, Lockfile, layout
│       ├── bake.compiler.cppm Toolchain detection, compile/link command gen, std module cache
│       ├── bake.moid.cppm     MoidDeclaration model, JSON codec, validation
│       ├── bake.graph.cppm    MoidGraph, MoidNode, dependency resolution
│       ├── bake.engine.cppm   source discovery, in-process module scan, DAG, executor
│       ├── bake.package.cppm  Resolver, Fetcher, Cache, lockfile
│       └── compiler/          in-process LLVM/Clang driver
│           ├── bake_llvm.cpp           LLVM init, in-process LLD linking
│           ├── bake_llvm.h
│           ├── bake_clang_driver.cpp   Clang driver shim (replaces external clang)
│           └── bake_clang_cc1_main.cpp cc1main entry for in-process compilation
├── lib/                  vendored runtime (distributed with bake)
│   ├── bake/
│   │   └── bake.build.cppm     build.cpp API (source-distributed, like libc++ headers)
│   ├── libcxx/                 vendored libc++ (include/, src/, modules/)
│   ├── libcxxabi/              vendored libc++abi (include/, src/)
│   ├── libunwind/              vendored libunwind (include/, src/)
│   ├── compiler-rt/            vendored compiler-rt (lib/)
│   ├── libc/darwin/            darwin SDK stubs (libSystem.tbd, SDKSettings.json, include/)
│   └── include/                vendored Clang headers
├── third_party/          vendored header-only libraries (as moid packages)
│   ├── nlohmann/               nlohmann/json (type = "lib")
│   └── tomlplusplus/           toml++ (type = "lib")
├── scripts/              build helpers
│   ├── build-llvm.sh           configure + build LLVM into external/
│   ├── fetch-darwin-headers.sh extract darwin SDK stubs
│   └── update-runtime.sh       update vendored libc++/libcxxabi/libunwind/compiler-rt
├── tests/
│   ├── test_runner.cpp   custom test framework (C++17), spawns bake binary
│   ├── graph_test.cpp    unit tests for graph/moid logic
│   └── projects/         fixture projects (55 test cases)
└── external/             LLVM source + prebuilt install (git submodule, not committed)
```

## Architecture

### Build pipeline (single path, not two)

Both convention mode and `build.cpp` mode produce the same `MoidDeclaration` JSON. After configure, everything converges:

1. **`resolve_moid_graph`** — resolve workspace + dependencies into a `MoidGraph` (topological moid order).
2. **`configure_moid_graph`** — for each moid: run `build.cpp` (if present) or convention scan → produce `MoidDeclaration`. Both paths write JSON to `out/.bake/`.
3. **`build_graph`** — translate all declarations into a flat `BuildAction` list (compile, compile_module, archive, link) with dependency edges.
4. **`execute_graph`** — parallel executor with content-based fingerprinting. Dirty detection + propagation, thread pool, incremental rebuilds.

### Integrated compiler toolchain

bake embeds LLVM and Clang as linked libraries (not subprocesses). The in-process driver replaces external `clang` and `ld`:

- **Compilation**: `bake_clang_driver.cpp` shims the Clang driver; `bake_clang_cc1_main.cpp` provides the cc1 entry. No `clang` binary is spawned.
- **Linking**: `bake_llvm.cpp` invokes LLD in-process. No system `ld`/`ld64` is used.
- **Module scanning**: text-based in-process scanner (`scan_module_file` in `bake.engine`). No `clang-scan-deps` subprocess.

### `import std` support

`ensure_std_modules()` in `bake.compiler` pre-builds `std.pcm` and `std.compat.pcm` from vendored libc++ sources, cached globally (`~/.cache/bake/<key>/std/`). The cache key incorporates compiler identity, content hashes of generated `.cppm` sources, and the vendored `LLVM_REVISION`.

### Static libc++

`ensure_libcxx_objects()` compiles libc++ + libc++abi from vendored sources into `.o` files, cached globally (`~/.cache/bake/libcxx-objects/`). These objects are injected into link actions to eliminate the `libc++.1.dylib` dependency.

### `bake.build` module

`lib/bake/bake.build.cppm` is the build.cpp API. It is source-distributed (like libc++ headers) and compiled fresh per project when a `build.cpp` is present. Context arrives via environment variables (`BAKE_MOID_ID`, `BAKE_SOURCE_DIR`, `BAKE_DEPS`, etc.). The script writes a `MoidDeclaration` JSON to `BAKE_DECLARATION_PATH`.

### Workspace

Multi-package projects like Cargo. Each member is a separate package with its own `bake.toml`. The root `bake.toml` declares `[workspace] members = [...]`. Workspace-internal dependencies use path deps and do NOT go through `.pkgs/` — only external (remote) sources are cached.

## Manifest Format (bake.toml)

```toml
[package]
name = "myapp"
version = "0.1.0"
type = "executable"          # executable (default) | lib | dylib
std = "c++23"                # c11 | c17 | c23 | c++17 | c++20 | c++23

[dependencies]
somelib = { path = "../somelib" }                          # path dependency (moid or raw source)
remotepkg = { url = "https://github.com/org/repo", tag = "v1.0" }  # remote (resolved by bake update)
```

### Moid types

- **`executable`** (default) — links into a runnable binary in `out/bin/`.
- **`lib`** — compiles sources + module interfaces, archives into `out/lib/lib<name>.a`. Public module PCMs and headers are passed to consumers. Objects are also linked into the consumer's terminal.
- **`dylib`** — links into a shared library in `out/lib/`.

## Output Layout

```
out/
├── bin/                 executables
├── lib/                 static/shared libraries
├── .obj/<moid-hash>/    per-moid object files
├── .bmi/<moid-hash>/    precompiled module interfaces (*.pcm)
└── .bake/               configure artifacts
    ├── graph.json            unified build DAG
    ├── fingerprints.json     content-based action fingerprints
    ├── <moid>.moid.json      declarations
    └── scripts/<hash>/       compiled build.cpp programs + bake.build.pcm
```

## Lockfile (bake.lock)

Identity-keyed, no version field, no `source` field. Keys:
- Moid package → moid name (e.g. `"brotli"`)
- Non-moid remote → resolved identity (e.g. `"git:https://github.com/curl/curl@68720b48..."`)
- Path dep → `"path:relative/path"`

```json
{
  "deps": {
    "brotli": { "path": "../brotli" },
    "curl": {
      "url": "https://github.com/curl/curl",
      "ref": "curl-8_21_0",
      "ref_type": "tag",
      "commit": "68720b483728...",
      "integrity": "sha256-b968947fdf73..."
    }
  }
}
```

## Testing

Tests are end-to-end: the test runner spawns the `bake` binary on fixture projects copied to temp dirs. No unit test framework — custom `CHECK(cond, msg)` / `CHECK_EQ(a, b, msg)` macros. 55 test cases covering builds, convention mode, build.cpp, workspaces, locking, archiving, and graph round-trips.

Test fixtures live in `tests/projects/`. When adding a new test, add a fixture + test case in `test_runner.cpp`.

## Build output format

bake uses cargo-style progress output:

```
  Downloading mbedtls          ← source downloads
  Downloading curl
   Building myapp v0.1.0       ← top-level header
   Compiling brotli v1.1.0     ← per-moid (once, when first action starts)
   Compiling myapp v0.1.0
    Finished in 3.6s           ← single summary line with timing
```

When nothing changed: just `   Building X` + `    Finished in 0.03s`. Per-file `[n/total] <path>` progress is behind `-v` / `--verbose`.

## Commit Messages

- **Conventional commits** — use `feat:`, `fix:`, `refactor:`, `chore:`, `docs:`, `test:`, `perf:` prefixes.
- **Subject only, no body** — keep it to a single line ≤ 50 characters. Only add a body if the subject genuinely can't capture the change.
- **Describe what changed, not why** — internal discussion, review feedback, and phase/iteration numbers don't belong in commit messages.
- **Examples**: `feat: in-process LLVM/Clang/LLD compiler`, `fix: skip std flags for C files in workspace`, `refactor: unify output layout to out/bin/lib/obj`.

## Key Constraints

- **Do NOT remove CMakeLists.txt** — it is the permanent Stage 0 bootstrap, not a temporary scaffold.
- **Do NOT freeze APIs** — the project is pre-release. Each phase should be implemented cleanly, not weighed down by backward compatibility.
- **No external toolchain dependencies** — bake embeds LLVM/Clang/LLD, vendors libc++/libc++abi/libunwind/compiler-rt, and uses in-process compilation/linking. The system Clang is only needed for the Stage 0 bootstrap.
- **User-facing output uses `macos`** not `darwin` for platform names.
- **`bake build` never moves a locked tag** — only `bake update` re-resolves tags to commits.
- **`--locked`** fails if lock is missing/stale; **`--offline`** bans network; **`--frozen`** = both.
- **Static by default** — link actions prefer static archives over shared libraries. Dynamic linking is reserved for future package distribution.
