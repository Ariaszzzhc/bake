# AGENTS.md — bake

## What is bake

bake is an all-in-one C/C++ build system, written in C++23. Think "Cargo for C++": convention-based with a `build.cpp` escape hatch, package management via `bake add`, and (future) an integrated compiler toolchain.

The project is pre-release. APIs are not frozen — make changes clean, not backward-compatible.

## Build & Test

```bash
# Bootstrap build (CMake + Ninja + Clang with libc++)
cmake -G Ninja -B build && cmake --build build

# Run the test suite (20 end-to-end tests)
ctest --test-dir build --output-on-failure

# Run a single test by name filter
./build/test_runner ./build/bake "workspace"

# Self-host: bake builds itself → out/
DYLD_LIBRARY_PATH=./build ./build/bake build

# Clean bake's own output
rm -rf out/
```

Requires: CMake ≥ 3.30, Ninja, Clang ≥ 19 with libc++ (Homebrew `llvm` formula on macOS).

## Language & Module Conventions

- **C++23**, strict standard (`CMAKE_CXX_EXTENSIONS OFF`), Clang + libc++.
- **`import std;`** — all standard library access is via `import std;`, not `#include`. The only `#include` directives in module files are for third-party headers (`<toml.hpp>`, `<nlohmann/json.hpp>`) and platform/OS headers (`<errno.h>`, `<sys/wait.h>`, `<windows.h>`), which go in the **global module fragment** (`module;` before `export module`).
- **I/O**: use `std::println(...)` for stdout, `std::println(std::cerr, ...)` for stderr, `std::print(...)` without trailing newline. Use `{}` format placeholders, not printf `%s`. Drop `.c_str()` — `std::format` handles `std::string` natively. Do NOT use `fprintf`, `printf`, or the bare `stderr`/`stdout` macros.
- **Module naming**: `bake.<subsystem>` (e.g. `bake.util`, `bake.engine`). File name mirrors module name: `bake.<name>.cppm`.
- **Module dependency chain**: `bake.util` → `bake.project` → `bake.compiler` → `bake.engine` / `bake.package` → `bake.cli`. Third-party modules (`nlohmann.json`, `tomlplusplus`) are consumed by core modules; `bake.cli` is in the `bake` executable, not `core`.

## Directory Layout

```
bake/                     workspace root
├── bake.toml             [workspace] members = ["core", "bake"]
├── CMakeLists.txt        Stage 0 bootstrap (permanent, not temporary)
├── core/                 shared library — compiler + engine + package + C ABI
│   ├── public/
│   │   └── bake.build.cppm    public API for build.cpp scripts (C ABI wrapper)
│   └── src/
│       ├── bake.util.cppm     Path, SHA-256, glob, process spawn
│       ├── bake.project.cppm  bake.toml model, Manifest, Lockfile, layout
│       ├── bake.compiler.cppm Toolchain detection, compile/link command gen
│       ├── bake.engine.cppm   source discovery, module scanning (P1689), DAG, executor
│       ├── bake.package.cppm  Resolver, Fetcher, Cache, lockfile
│       └── cabi/
│           ├── bake_cabi.h    C ABI header (opaque handles, extern "C")
│           └── api.cpp        C ABI implementation
├── bake/                 executable — CLI + main
│   └── src/
│       ├── main.cpp           import bake.cli; → bake::cli::main()
│       └── cli.cppm           CLI dispatch, all commands (init/build/add/update/run/clean)
├── tests/
│   ├── test_runner.cpp   custom test framework (C++17), spawns bake binary
│   └── projects/         fixture projects (simple_app, static_lib, path_dep, etc.)
└── third_party/          vendored header-only (toml++, nlohmann/json)
    ├── nlohmann/
    │   ├── public/nlohmann/json.hpp
    │   └── modules/json.cppm         official C++ module wrapper
    └── tomlplusplus/
        ├── public/toml++/toml.hpp
        └── modules/tomlplusplus.cppm  official C++ module wrapper
```

## Architecture

- **C ABI** (`bake_cabi.h` / `api.cpp`): opaque handles (`bake_builder*`, `bake_target*`, etc.) with `extern "C"` functions. All functions are `noexcept` — exceptions caught internally, converted to return codes + thread-local `bake_last_error()`. This ABI exists so user `build.cpp` scripts can link `libbake` (the core shared library, output name `bake`) without consuming C++23 modules.
- **bake.build.cppm**: thin C++ wrapper over the C ABI. Distributed as source, compiled fresh per project by the engine when a `build.cpp` is present.
- **Engine build flow**: discover sources → scan with `clang-scan-deps` (P1689) → build module DAG → topological sort → compile module interfaces → compile sources → link.
- **import std support**: the compiler module pre-builds both `std.pcm` and `std.compat.pcm` from vendored libc++ sources (`ensure_std_modules()` in `bake.compiler`), caches them in the project output tree (`out/.bmi/.std/`), and injects `-fmodule-file=std=` / `-fmodule-file=std.compat=` into all C++ compile actions. The cache key incorporates compiler identity, content hashes of both generated `.cppm` sources, and the vendored `LLVM_REVISION`.
- **Workspace**: multi-package projects like Cargo. Each member is a separate package with its own `bake.toml`. The root `bake.toml` declares `[workspace] members = [...]`.
- **External source builds**: bake has no built-in CMake/Meson integration and never infers a foreign build graph. `bake add` resolves source; `build.cpp` explicitly describes how that source is compiled.

## Manifest Format (bake.toml)

```toml
[moid]
name = "myapp"
version = "0.1.0"
type = "executable"          # or "lib", "dylib"
std = "c++23"                # c++17 | c++20 | c++23

[dependencies]
somelib = { path = "../somelib" }              # path dependency
remotepkg = { url = "https://.../repo", tag = "v1.0" }  # remote (resolved by bake add)
```

## Output Layout

```
out/
├── bin/          executables
├── lib/          shared/static libraries
├── obj/          per-member object files (out/obj/{member_name}/*.o)
└── bmi/          precompiled module interfaces (*.pcm)
```

## Testing

Tests are end-to-end: the test runner spawns the `bake` binary on fixture projects copied to temp dirs. No unit test framework — custom `CHECK(cond, msg)` / `CHECK_EQ(a, b, msg)` macros.

Test fixtures live in `tests/projects/`. When adding a new test, add a fixture + test case in `test_runner.cpp`.

## Key Constraints

- **Do NOT remove CMakeLists.txt** — it is the permanent Stage 0 bootstrap, not a temporary scaffold.
- **Do NOT freeze APIs** — the project is pre-release. Each phase should be implemented cleanly, not weighed down by backward compatibility.
- **No external runtime dependencies** — SHA-256, glob, HTTP downloads (via `curl` subprocess), archive extraction (via `tar` subprocess) are all hand-implemented or spawned. Only vendored third-party headers (toml++, nlohmann/json) are linked.
- **User-facing output uses `macos`** not `darwin` for platform names.
- **`bake build` never moves a locked tag** — only `bake update` re-resolves tags to commits.
- **`--locked`** fails if lock is missing/stale; **`--offline`** bans network; **`--frozen`** = both.
