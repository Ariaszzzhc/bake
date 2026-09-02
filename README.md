# bake

**A C/C++ build system, package manager, and compiler toolchain — in one binary.**

Write code, run `bake build`, get a binary. No Makefile, no CMakeLists, no
`brew install llvm`, no sysroot assembly, no "works on my machine".

[中文说明](README.zh.md)

## What is this?

bake is three things welded into one executable:

- **A build system.** Convention over configuration: a `bake.toml` next to
  `src/` is a complete build definition. When conventions run out, a
  `build.cpp` script describes the inputs — in real C++, not another DSL.
- **A package manager.** Git and archive dependencies, path dependencies,
  workspaces, a lockfile, and `--frozen` for CI.
- **A compiler toolchain.** bake embeds LLVM, Clang, and LLD, and vendors
  libc++, compiler-rt, musl, MinGW-w64, and the glibc link surface. It
  compiles and links in-process: no compiler subprocesses, no system
  toolchain, ever.

Why bother? Because in C and C++ the toolchain itself is ambient state —
which clang you get depends on the machine, the package manager, and the
phase of the moon. bake pins the entire toolchain inside the build tool, so
a build that works on your machine works on every machine, and
cross-compiling is a flag instead of a project.

The inspiration list is short and honest: [Cargo] for the ergonomics —
conventions, workspaces, lockfiles — and [zig cc] for the idea that a
compiler can be a single self-contained binary that targets everything.

[Cargo]: https://doc.rust-lang.org/cargo/
[zig cc]: https://ziglang.org/download/#zig-cc

## A taste

```
$ bake init hello && cd hello
$ bake build
   Building hello v0.1.0
   Compiling hello v0.1.0
    Finished in 1.1s
$ bake run
Hello from hello!
```

Cross-compiling is the same command with a target:

```
$ bake build -t x86_64-linux-musl
   Building hello v0.1.0
   Compiling hello v0.1.0
    Finished in 1.1s
$ file out/x86_64-linux-musl/bin/hello
out/x86_64-linux-musl/bin/hello: ELF 64-bit LSB executable, x86-64, ... statically linked
```

No toolchain files, no SDK, no `-DCMAKE_TOOLCHAIN_FILE`. The first build for
a new target compiles that target's C runtime from vendored source and
caches it; everything after that is incremental.

C++20 modules and `import std;` work out of the box — bake pre-builds the
`std` module from its own libc++ and caches it per target:

```cpp
import std;
import math;

int main() {
    std::println("{}", triple(14));    // 42
}
```

And when default discovery isn't enough, `build.cpp` says what to compile:

```cpp
import bake;

int main() {
    bake::Builder b;
    b.sources({"src/*.cpp", "src/platform/*.cpp"});
    b.public_headers("include");
    return b.build();
}
```

The script describes *inputs*; `bake.toml` still owns configuration
(profiles, flags, link libraries, per-target settings). See the
[docs](docs/) for the whole story.

## Supported platforms

Hosts: macOS (aarch64, x86_64), Linux (aarch64, x86_64), Windows (x86_64).

Cross-compile targets, from any host:

| Target triple | libc | Arch |
|---|---|---|
| `*-apple-darwin` | host libSystem (bake ships the SDK stubs) | aarch64, x86_64 |
| `*-linux-musl` | musl, built from vendored source | aarch64, x86_64 |
| `*-linux-gnu` | glibc: vendored headers + synthesized link stubs | x86_64, aarch64 |
| `*-windows-gnu` | MinGW-w64 v14, built from vendored source | x86_64, aarch64 |

gnu targets pick a baseline with a triple suffix (`-t x86_64-linux-gnu.2.36`)
and link against the real glibc at runtime; everything else links statically
by default.

Also on board: `import std;` and named modules on every target, asan/ubsan
on native builds, LLD in-process for all three object formats, and
`bake cc` / `bake c++` / `bake ar` if you want the embedded toolchain `zig
cc`-style, directly.

## Getting bake

There are no binary releases yet — build it (once, with any system Clang):

```bash
git clone --recursive <bake-repo-url> bake
cd bake

# Stage 0: bootstrap with the system compiler.
# Needs CMake >= 3.30, Ninja, Clang >= 22 with libc++.
cmake -G Ninja -B build && cmake --build build

# Stage 1: bake builds itself.
./build/bake build
```

`out/<host-triple>/bin/bake` is the self-hosted binary — from here on it
needs nothing from the system but the kernel. Verify with `bake audit`,
which checks that builds really are self-contained.

To cross-compile bake itself: `./bootstrap/build x86_64-linux-musl` — it
rebuilds LLVM for the target with bake's own compiler first.

To package a distributable archive (release binary + vendored runtime,
zig-style, unpack and go): `./bootstrap/package x86_64-linux-musl` —
output lands in `dist/`.

```
bake init [name]     scaffold a project
bake build           build it (incremental, parallel)
bake run [args…]     build if needed, then run
bake test [name]     build and run registered tests
bake add <url>       add a dependency (--tag / --branch / --rev)
bake remove <name>   remove one
bake update [dep]    re-resolve locked refs (the only command that moves them)
bake clean           remove out/
bake cc / c++ / ar   the embedded toolchain, directly
bake audit           verify toolchain self-containment
```

Useful flags: `-t <triple>`, `--release` / `--profile <name>`, `-p <member>`,
`-j <n>`, `--option <name>[=value]`, `--locked`, `--offline`, `--frozen`.
Full reference in the docs.

## Documentation

bake ships a bilingual usage book:

- [English](docs/en/README.md)
- [中文](docs/zh/README.md)

Start with [First Steps](docs/en/getting-started/first-steps.md), then read
about [dependencies](docs/en/guide/dependencies.md),
[cross-compilation](docs/en/guide/cross-compilation.md), and
[build.cpp](docs/en/guide/build-scripts.md).

## License

[MIT](LICENSE). The vendored sources under `lib/` and `third_party/` keep
their upstream licenses (LLVM, Darwin, musl, MinGW-w64, glibc, compiler-rt,
nlohmann/json, toml++).
