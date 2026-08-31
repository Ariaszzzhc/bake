# Output and Cache Layout

## Project output: `out/`

All build products live under the project root, one subtree per target triple:

```
out/
├── aarch64-apple-darwin/      # native in this example
│   ├── bin/                   # executables (main + build.cpp binaries)
│   ├── lib/                   # lib → .a, dylib → .dylib/.so/.dll
│   ├── .obj/                  # object files
│   ├── .bmi/                  # module PCM files
│   └── .bake/                 # declarations, fingerprints, graph.json
└── .bake/                     # host-level artifacts (compiled build.cpp scripts)
```

- `bin/` holds the moid's executable plus every `b.binary()` target.
- `lib/` holds `lib<name>.a` for libs and the shared library for dylibs. Dependents link the archive and receive its public headers/modules.
- `.bake/` inside a triple stores the resolved `MoidDeclaration` JSON per moid, content fingerprints, and the action graph — this is what makes rebuilds incremental. Deleting it forces re-resolution (not recompilation of sources whose objects still fingerprint clean).

`bake clean` removes `out/` entirely.

## Global cache: `~/.cache/bake/`

Shared across projects, keyed by toolchain/content:

| Path | Contents |
|---|---|
| `~/.cache/bake/<key>/std/` | Pre-built `std.pcm` per (target, std version) for `import std;` |
| `~/.cache/bake/libcxx-objects/` | Static libc++ / libc++abi / libunwind objects, per target |
| (package cache) | Fetched remote dependencies (git checkouts) |

Cross-target libc builds (musl CRT, MinGW-w64 CRT + winpthreads) are also cached per target under the same scheme, so the first `bake build -t x86_64-linux-musl` pays the runtime cost once.

## Fingerprinting and incrementality

- Every build action is fingerprinted by content: inputs, flags, defines, target, profile.
- A change to one source recompiles that TU and its dependents (module dependency edges included) — nothing else.
- Profile and target changes produce different fingerprints and different output directories; switching back and forth never thrashes.
- `build.cpp` scripts are re-run only when the script, `bake.toml`, or the `bake.build` module is newer than the cached declaration.

## What bake never does

- Never writes outside `out/` and `~/.cache/bake/` (plus `bake.lock`/`bake.toml` edits from `add`/`update`).
- Never moves a locked dependency tag during `build`.
- Never requires a clean build: cache corruption is the only reason to `rm -rf out/`.
