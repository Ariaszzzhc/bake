# bake

**An all-in-one C/C++ build system and compiler toolchain.**

One binary, one command. No system clang, no system libc++, no system linker, no Makefiles.

---

## What is bake

bake is a build tool, a package manager, and a complete compiler — bundled into a single executable. It compiles C and C++ source code using an embedded LLVM/Clang toolchain, links with an embedded LLD, and vendors its own libc++, libc++abi, libunwind, compiler-rt, and libc. The result: a reproducible build environment that works identically on any machine, with zero external dependencies.

### Vision

Building C and C++ should be as simple as `cargo build` in Rust. You write code, you run one command, you get a binary. No hunting for the right compiler version, no wrangling CMake presets, no `brew install llvm`, no "works on my machine."

bake makes that real. Every bake installation ships the same compiler, the same standard library, and the same libc — regardless of what's installed on the host. Cross-compilation to macOS, Linux, and Windows works out of the box, from any host.

### Inspirations

- **Cargo** (Rust) — convention-based builds, workspace model, package management, lockfiles.
- **zig cc** — a C/C++ compiler distributed as a single binary with its own libc, capable of cross-compiling to any target from any host.

bake combines both ideas: Cargo's ergonomics with zig cc's self-contained toolchain, for C and C++.

## Supported Platforms

### Host (where bake runs)

| OS | Arch |
|---|---|
| macOS | aarch64 (Apple Silicon), x86_64 (Intel) |

### Cross-compile targets

| Target triple | libc | Arch |
|---|---|---|
| `aarch64-darwin`, `x86_64-darwin` | host libSystem (macOS) | aarch64, x86_64 |
| `aarch64-linux-musl`, `x86_64-linux-musl` | musl (vendored source) | aarch64, x86_64 |
| `x86_64-windows-gnu`, `aarch64-windows-gnu` | MinGW-w64 v14 (vendored source) | x86_64, aarch64 |

All three libc families are self-contained — no SDK, no sysroot, no NDK needed.

## Features

### Convention-based builds

Drop a `bake.toml` next to `src/` and bake does the rest. It discovers sources, compiles them, links the result. No build script required.

### The `build.cpp` escape hatch

When conventions aren't enough, write a `build.cpp`:

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

bake compiles and runs this script during configure. Both paths produce the same internal representation — no second-class citizen.

### Cross-compilation

```bash
bake build -t x86_64-linux-musl
bake build -t x86_64-windows-gnu
```

No extra toolchain to install. bake vendors musl and MinGW-w64 source and builds the CRT from source on demand.

### Package management

```toml
[dependencies]
curl = { url = "https://github.com/curl/curl", tag = "curl-8_21_0" }
```

`bake update` resolves tags to commits, downloads sources, and locks them. Dependencies don't need to be bake projects — a `build.cpp` wrapper is all it takes to consume any C/C++ library.

### `import std` support

Full C++23 module support including `import std;`. bake pre-builds `std.pcm` from vendored libc++ sources and caches it globally.

### Incremental builds

Content-based fingerprinting — change a source file and only the affected actions re-run. Touch a file without changing content and nothing rebuilds.

## Getting Started

### Build bake from source (Stage 0)

Requires: CMake ≥ 3.30, Ninja, Clang ≥ 19 with libc++.

```bash
git clone --recursive https://github.com/arias/bake-compiler.git
cd bake-compiler
cmake -G Ninja -B build && cmake --build build
```

### Self-host (Stage 1)

bake builds itself with its own integrated compiler:

```bash
./build/bake build
```

`out/bin/bake` is fully self-contained — no system toolchain dependencies.

### Create a project

```bash
bake init myapp
cd myapp
bake build
./out/bin/myapp
```

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

Build flags: `-t <triple>` (cross-compile target), `-j <n>` (parallelism), `-p <member>` (workspace member), `-v` (verbose), `--locked`, `--offline`, `--frozen`.

---

[中文](README.zh.md)
