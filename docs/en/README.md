# The bake Book

bake is an all-in-one C/C++ build system and compiler toolchain. Think "Cargo for C++" meets `zig cc`: default source discovery, programmable `build.cpp` input declarations, package management, and an integrated LLVM/Clang toolchain — all in one binary, with no external compiler or toolchain dependency.

- No Makefile, no CMakeLists, no generator step. `bake build` is the whole pipeline.
- The compiler is embedded. bake links LLVM + Clang + LLD as libraries; every compiler, assembler, archiver, and linker invocation stays in the bake binary, while Core may use Bake-owned child processes for scheduled work. No system compiler toolchain is required after bootstrap.
- C++20 modules (and `import std;`) work out of the box, including cross-compilation.

This book is organized like the Cargo book:

**Getting Started**

- [Installation](getting-started/installation.md)
- [First Steps](getting-started/first-steps.md) — init, build, run, project layout

**Guide**

- [Dependencies](guide/dependencies.md) — path deps, remote deps, the lockfile
- [Workspaces](guide/workspaces.md)
- [Programmable Inputs](guide/build-scripts.md) — `build.cpp`, the three input tiers
- [Testing](guide/testing.md) — `add_test`, `bake test`
- [Cross-Compilation](guide/cross-compilation.md)

**Reference**

- [Manifest Format](reference/manifest.md) — every `bake.toml` key
- [Profiles and Options](reference/profiles.md)
- [build.cpp API](reference/builder-api.md) — the `bake::Builder` class
- [Command-Line Interface](reference/cli.md)
- [Output and Cache Layout](reference/output-and-cache.md)
