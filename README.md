# bake

bake is a build system and compiler toolchain for C and C++ that doesn't need anything else installed.

Think Cargo for C++, crossed with `zig cc`. You get a build tool, a package manager, and a complete compiler — all in one binary. No system clang, no system libc++, no system linker. bake ships its own.

## Why

Building C and C++ projects is painful. You need the right compiler, the right linker, the right standard library, a build system, a dependency manager, and they all have to agree on versions. If you've ever tried to set up a CI pipeline for a C++ project, you know the feeling.

bake makes it simple. One binary, one command, done.

```bash
bake build
```

That's it. bake will compile your sources, resolve your dependencies, link everything, and put the result in `out/bin/`. No Makefiles, no CMakeLists, no `brew install llvm`.

## Getting started

### Build bake from source (Stage 0)

bake bootstraps itself with CMake + your system Clang. This is the only time you need an external compiler.

```bash
git clone --recursive https://github.com/arias/bake-compiler.git
cd bake-compiler
cmake -G Ninja -B build && cmake --build build
```

Now you have `./build/bake` — a working bake built with the system toolchain.

### Self-host (Stage 1)

bake can build itself using its own integrated compiler and vendored libc++:

```bash
./build/bake build
```

The result in `out/bin/bake` is fully self-contained — no system toolchain dependencies.

### Create a new project

```bash
bake init myapp
cd myapp
bake build
./out/bin/myapp
```

### Project layout (convention mode)

If your project follows the convention, you don't need a build script at all:

```
myapp/
├── bake.toml
└── src/
    └── main.c
```

```toml
[package]
name = "myapp"
version = "0.1.0"
```

bake discovers `src/`, compiles everything, and links an executable. Put headers in `public/` to share them with dependents.

### The build.cpp escape hatch

When conventions aren't enough, drop a `build.cpp` next to `bake.toml`:

```cpp
import bake.build;

int main() {
    bake::Builder builder;
    builder.executable("myapp")
        .sources("src/*.cpp")
        .sources("src/platform/*.cpp", {
            .defines = {"PLATFORM_LINUX"}
        })
        .include_dirs("include")
        .std("c++23");
    return builder.build();
}
```

bake compiles and runs this script during its configure phase. The builder API is source-distributed (like a header) — no linking against bake internals.

## Dependencies

### Path dependencies

```toml
[dependencies]
mylib = { path = "../mylib" }
```

Local packages with their own `bake.toml`. Workspace members resolve instantly; no copying, no caching.

### Remote dependencies

```toml
[dependencies]
curl = { url = "https://github.com/curl/curl", tag = "curl-8_21_0" }
```

Run `bake update` to resolve tags to commits, download sources, and lock them in `bake.lock`. Subsequent `bake build` uses the locked versions. If the source cache is missing, bake re-downloads automatically using the locked commits.

### Non-moid packages

A dependency doesn't need to be a bake project. If it's just source code, provide a `build.cpp` alongside it (or in your project) that tells bake how to compile it. This is how bake wraps existing C/C++ libraries — no CMake or Meson integration needed.

## What's inside

bake is a single executable with no external runtime dependencies:

- **LLVM + Clang** — compiled in-process. No `clang` subprocess.
- **LLD** — linked in-process. No system `ld` or `ld64`.
- **libc++ + libc++abi** — vendored as source, compiled on demand and cached. No `libc++.1.dylib`.
- **libunwind, compiler-rt** — vendored.
- **Darwin SDK stubs** — `libSystem.tbd` and friends shipped with bake. No Xcode needed for linking.

On macOS, the only system dependency is the kernel-provided `libSystem.B.dylib` (which every macOS binary needs). Everything else comes from bake.

## Moid types

A "moid" (模具, Chinese for mold/template) is bake's unit of compilation. Each `bake.toml` describes one moid:

| Type | What it produces | What consumers get |
|------|-----------------|-------------------|
| `executable` (default) | A binary in `out/bin/` | — |
| `lib` | A static archive in `out/lib/` + module PCMs | Public headers, module interfaces, linked objects |
| `dylib` | A shared library in `out/lib/` | Public headers, module interfaces |

## Build output

```
out/
├── bin/               your executables
├── lib/               static/shared libraries
├── .obj/              object files (content-hashed)
├── .bmi/              precompiled module interfaces
└── .bake/             configure artifacts (DAG, fingerprints, scripts)
```

Incremental builds use content-based fingerprinting. Change a source file and only the affected actions re-run. Touch a file without changing its content and nothing rebuilds.

## Commands

```
bake init [name]           Create a new project scaffold
bake build                 Build the project
bake run                   Build and run the default executable
bake clean                 Remove out/
bake add <url> --tag <t>   Add a remote dependency
bake update                Re-resolve tags to commits, update lock
bake test                  Build and run tests (if configured)
```

Build flags: `-j <n>` (parallelism), `-p <member>` (workspace member), `-v` (verbose per-file progress), `--locked`, `--offline`, `--frozen`.

## Status

Pre-release. Everything works but APIs are not frozen. Don't build production pipelines on this yet — things will change.

## Requirements

**Stage 0 bootstrap**: CMake ≥ 3.30, Ninja, Clang ≥ 19 with libc++.

**Stage 1+ (self-hosted)**: Just bake. That's the whole point.
