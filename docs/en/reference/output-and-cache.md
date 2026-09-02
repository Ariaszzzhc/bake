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
│   └── .bake/                 # declarations, fingerprints
└── .bake/                     # host-level artifacts (compiled build.cpp scripts)
```

- `bin/` holds the moid's executable plus every `b.binary()` target.
- `lib/` holds `lib<name>.a` for libs and the shared library for dylibs. Dependents link the archive and receive its public headers/modules.
- `.bake/` inside a triple stores the resolved build declarations, content fingerprints, and the action graph — this is what makes rebuilds incremental. Deleting it forces re-resolution (not recompilation of sources whose objects still fingerprint clean).

`bake clean` removes `out/` entirely.

## Global cache: `~/.cache/bake/`

Shared across projects, keyed by target and content:

| Path | Contents |
|---|---|
| `~/.cache/bake/<triple>/` | Toolchain products for that target: C runtime objects, the `std` module, libc++ objects, sanitizer runtimes — all content-addressed |
| `~/.cache/bake/src/` | Fetched remote dependency sources (git checkouts, extracted archives) |

The first `bake build -t <triple>` pays the runtime build cost once; every later build — in any project — reuses the cached products. Targets with a version suffix (`x86_64-linux-gnu.2.36`) get their own cache entry, separate from the unsuffixed triple.

## Fingerprinting and incrementality

- Every build action is fingerprinted by content: inputs, flags, defines, target, profile.
- A change to one source recompiles that TU and its dependents (module dependency edges included) — nothing else.
- Profile and target changes produce different fingerprints and different output directories; switching back and forth never thrashes.
- `build.cpp` scripts are re-run only when the script, `bake.toml`, or the `bake.build` module is newer than the cached declaration.

## What bake never does

- Never writes outside `out/` and `~/.cache/bake/` (plus `bake.lock`/`bake.toml` edits from `add`/`update`).
- Never moves a locked dependency tag during `build`.
- Never requires a clean build: cache corruption is the only reason to `rm -rf out/`.
