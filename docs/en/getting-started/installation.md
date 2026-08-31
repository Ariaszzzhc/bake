# Installation

## Requirements

Bootstrapping bake from source requires:

- CMake ≥ 3.30
- Ninja
- Clang ≥ 19 with libc++

These are only needed once: the bootstrap compiler builds stage 0, and stage 0 builds everything after that. The resulting bake binary has no external toolchain dependencies — it embeds LLVM/Clang/LLD and vendors libc++, libc++abi, libunwind, compiler-rt, musl, and MinGW-w64.

## Build from source

```bash
git clone <bake-repo-url> bake
cd bake

# Stage 0: bootstrap with the system Clang
cmake -G Ninja -B build
cmake --build build

# Stage 1: bake builds itself (verifies self-hosting)
./build/bake build
```

The stage-0 binary lands in `build/bake`; the self-hosted binary in `out/<triple>/bin/bake`. Both are full-featured. Add one of them to your `PATH`.

```bash
bake --version
```

## Verifying the toolchain

```bash
bake audit
```

`bake audit` verifies that bake's compiler produces binaries without relying on system SDK headers or libraries beyond the kernel interface. Run it after a fresh build if you want proof of self-containment.

## Cross-compiling bake itself

The `bootstrap/` directory contains the stage-0 script used to cross-compile bake to any of its supported targets. The everyday way to produce a bake binary for another platform is bake itself:

```bash
bake build -t x86_64-linux-musl
```

## Updating

bake vendors its runtime (libc++, compiler-rt, …). Scripts under `scripts/` (`update-runtime.sh`, `fetch-musl.sh`, `fetch-mingw.sh`, …) refresh those sources; rebuild afterwards.
