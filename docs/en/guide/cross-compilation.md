# Cross-Compilation

bake cross-compiles from any host to three libc families — no sysroot assembly, no toolchain files, no external cross-compiler.

```bash
bake build -t x86_64-linux-musl
bake build -t x86_64-windows-gnu
bake build -t aarch64-apple-darwin
```

## Supported targets

| Target triple | libc | Architectures |
|---|---|---|
| `*-apple-darwin` | host libSystem (macOS), bake ships SDK stubs | aarch64, x86_64 |
| `*-linux-musl` | musl (vendored source, built from source) | aarch64, x86_64 |
| `*-windows-gnu` | MinGW-w64 v14 (vendored source) | x86_64, aarch64 |

The first build for a target compiles the vendored libc from source and caches it; subsequent builds are incremental like everything else. `import std;` and C++ modules work identically on every target — the `std.pcm` and libc++ objects are built per-target in the cache.

## How it works

Everything needed is embedded in the bake binary:

1. The libc family is resolved from the triple (`resolve_libc_family`).
2. The runtime is prepared: Darwin uses shipped `libSystem.tbd` stubs (no Xcode needed); musl and MinGW-w64 CRT + winpthreads are compiled from vendored source into static archives.
3. Per-target compile/link commands are generated with the right sysroot and driver flavor; LLD is invoked in-process (Darwin, COFF/MinGW, or GNU flavor).
4. MinGW import libraries are generated on demand from `.def` files — only for libraries you actually reference with `-l`.

Linking prefers static archives — cross binaries are self-contained by default.

## Per-target configuration in `bake.toml`

```toml
[target."*-apple-darwin"]
frameworks = ["Foundation"]

[target."*-linux-musl"]
libraries = ["pthread", "dl"]

[target."*-windows-gnu"]
libraries = ["ws2_32"]
```

`[target."<glob>"]` sections apply when the active triple matches. The glob is segment-based: `*` matches one complete `-`-separated segment, so `*-apple-darwin` matches `aarch64-apple-darwin` but `*-darwin` matches nothing. Multiple sections can match one triple; they merge.

## Running and testing

Cross-built binaries land in `out/<triple>/bin/`. `bake run` and `bake test` select the same directory via `-t`, so on a mac:

```bash
bake build -t x86_64-linux-musl    # produces a Linux binary
bake test -t x86_64-linux-musl     # builds for the target; running happens
                                   # only if the host can execute it
```

bake does not provide an emulator; run foreign binaries with your own (qemu, wine, Docker).

## Output isolation

Each target gets its own subtree (`out/<triple>/…`), so switching targets never invalidates other targets' incremental state — build for the host, cross-build for two others, and all three trees stay warm.
