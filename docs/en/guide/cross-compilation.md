# Cross-Compilation

bake cross-compiles from any host to four libc families — no sysroot assembly, no toolchain files, no external cross-compiler.

```bash
bake build -t x86_64-linux-musl
bake build -t x86_64-linux-gnu
bake build -t x86_64-windows-gnu
bake build -t aarch64-apple-darwin
```

## Supported targets

| Target triple | libc | Architectures |
|---|---|---|
| `*-apple-darwin` | host libSystem (macOS), bake ships SDK stubs | aarch64, x86_64 |
| `*-linux-musl` | musl (vendored source, built from source) | aarch64, x86_64 |
| `*-linux-gnu` | glibc (source subset + per-version synthesized link stubs) | x86_64, aarch64 |
| `*-windows-gnu` | MinGW-w64 v14 (vendored source) | x86_64, aarch64 |

The first build for a target compiles the vendored libc from source and caches it; subsequent builds are incremental like everything else. `import std;` and C++ modules work identically on every target — the `std.pcm` and libc++ objects are built per-target in the cache.

## glibc targets (`*-linux-gnu`)

gnu targets produce **dynamically linked** ELFs that run against the real glibc on the target machine. The default baseline is glibc 2.28; pick another version (≥ 2.28) with a triple suffix:

```bash
bake build -t x86_64-linux-gnu          # default 2.28
bake build -t x86_64-linux-gnu.2.36     # explicit version
```

The version suffix governs both the **link surface** and the **header surface**: the vendored headers come from the newest glibc release, and `__GLIBC__`/`__GLIBC_MINOR__` are pinned to the target version at compile time, so `__GLIBC_PREREQ` gates open and close per target — a `.2.36` target can use `gettid()` (added in glibc 2.30) and links against `gettid@GLIBC_2.30`, while a `.2.28` target rejects it at compile time.

How: the crt objects and `libc_nonshared.a` are compiled from the vendored glibc source subset; the libc itself is never built — link-time stub `.so` files are synthesized per target version from the vendored `abilists` symbol/version table, with `--as-needed` keeping only the libraries actually used (a hello world ends up with `DT_NEEDED: libc.so.6` only). Static linking is rejected on gnu targets (glibc's static mode has known dlopen/NSS defects) — use a musl target for static builds.

## Sanitizers

`bake cc` / `bake c++` and `bake build` (`[profile.*] sanitize = [...]`) support `-fsanitize=undefined` (UBSan standalone) and `-fsanitize=thread` (TSan): the runtimes are compiled from the vendored compiler-rt sources per target on first link and cached globally. ELF targets only (linux-gnu / linux-musl) for now. `-fsanitize=address` is not vendored: compile-only invocations still instrument; the link is rejected with a clear message.

## How it works

Everything needed is embedded in the bake binary:

1. The libc family is resolved from the triple (`resolve_libc_family`).
2. The runtime is prepared: Darwin uses shipped `libSystem.tbd` stubs (no Xcode needed); musl and MinGW-w64 CRT + winpthreads are compiled from vendored source into static archives; gnu compiles crt/`libc_nonshared.a` and synthesizes the stub libraries.
3. Per-target compile/link commands are generated with the right sysroot and driver flavor; LLD is invoked in-process (Darwin, COFF/MinGW, or GNU flavor).
4. MinGW import libraries are generated on demand from `.def` files — only for libraries you actually reference with `-l`.

musl and MinGW linking prefers static archives; gnu targets always link dynamically.

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

Each target gets its own subtree (`out/<triple>/…`), so switching targets never invalidates other targets' incremental state — build for the host, cross-build for three others, and all four trees stay warm.
