# Output and Cache Layout

## Project output: `out/`

All build products live under the project root, one subtree per full target
triple and profile:

```
out/
├── aarch64-apple-darwin/      # native in this example
│   ├── dev/
│   │   ├── bin/               # executables (main + build.cpp binaries)
│   │   ├── lib/               # lib → .a, dylib → .dylib/.so/.dll
│   │   ├── .obj/              # object files
│   │   ├── .bmi/              # module PCM files
│   │   └── .bake/             # project state, action receipts, LSP data
│   └── release/               # an independent release context
└── ...
```

- `bin/` holds the moid's executable plus every `b.binary()` target.
- `lib/` holds `lib<name>.a` for libs and the shared library for dylibs. Dependents link the archive and receive its public headers/modules.
- `.bake/` inside a target/profile context stores the shared project snapshot,
  build action receipts, and analysis data. Its Core state is isolated under
  `.bake/core/<context-key>/`: immutable snapshots live at
  `snapshots/<sha256>.json`, the digest-protected last successful observation is
  `latest.json`, completed request records live at `results/<request-id>.json`,
  and in-progress work is staged in `work/`. Build publication
  is serialized by `.bake/build-publication.lock` in the same context.

`bake clean` removes `out/` entirely.

Dirty build actions write to the current request's Core `work/` directory.
Before publication, bake takes the target/profile publication lock and rehashes
the captured strong inputs. If they still match, planned products replace their
destinations atomically and the immutable ready result is published last. A
cached action is reusable only when its successful request record and each
current product's digest and size still match.

Before `run` or `test` starts a generated executable, bake verifies the
successful publication of its build result. It then reads and captures the
selected executable and the generated project shared libraries in its dependency
closure. Every captured file must match the result's digest and size. The process
runs from this private
`bin`/`lib` layout, so a concurrent replacement of the canonical output cannot
change the running invocation. Bake removes the capture after the process
exits. Prebuilt and external shared libraries remain outside this capture.

Generated dylibs use stable runtime-relative identities within the project
layout. On macOS, the default identity is `@rpath/<filename>` and consumers use
`@loader_path/../lib`; on ELF targets, the default SONAME is `<filename>` and
consumers use `$ORIGIN/../lib`. An explicit install name or SONAME is preserved.
On Windows, generated DLL dependencies are captured beside the executable.

The language server keeps context-local analysis products inside the project,
next to the other build intermediates. The
`out/<full-target>/<profile>/.bake/lsp-preamble/` tree stores content-addressed
preamble PCHs and JSON receipts. A PCH object identity covers the Core context,
analysis command and compiler, exact preamble prefix, and captured dependency
identities; its receipt revalidates the context, dependencies, prefix, bounds,
and product path before reuse. The
`out/<full-target>/<profile>/.bake/lsp-index/` tree stores immutable JSON symbol
shards per translation unit. Each shard key covers the compilation and compiler
plus captured source and dependency identities; malformed or mismatched shards
are rejected and rebuilt. Living under `out/`
means `bake clean` clears both stores with everything else, and temporary
projects take their analysis products with them.

## Global cache: `~/.cache/bake/`

Shared across projects, keyed by target and content:

| Path | Contents |
|---|---|
| `~/.cache/bake/<triple>/` | Toolchain products for that target: C runtime objects, the `std` module, libc++ objects, sanitizer runtimes — all content-addressed |
| `~/.cache/bake/src/` | Fetched remote dependency sources (git checkouts, extracted archives) |

The first `bake build -t <triple>` pays the runtime build cost once; every later build — in any project — reuses the cached products. Targets with a version suffix (`x86_64-linux-gnu.2.36`) get their own cache entry, separate from the unsuffixed triple.

Toolchain products are bounded: after each insertion the target namespace is
trimmed to a least-recently-used quota (8 GiB by default, override with the
`BAKE_CACHE_LIMIT_MB` environment variable). Eviction only removes
content-addressed product directories — a removed product simply rebuilds on
the next miss, and the source cache under `src/` is never trimmed.

## Fingerprinting and incrementality

- Every build action is fingerprinted by content: inputs, flags, defines, target, profile.
- A change to one source recompiles that TU and its dependents (module dependency edges included) — nothing else.
- Profile and target changes select different output directories and separate
  project-state contexts.
- The Core context identity also includes the canonical workspace, the selected
  member (or all members), sorted features, and the running bake compiler's
  canonical path, version, and content digest.
- A project snapshot records the canonical path, existence, and exact filesystem
  modification time of every relevant workspace/member `bake.toml`, the workspace's
  actual `bake.lock` path (including its absence), and every participating
  `build.cpp`. Declaration state is reused only when that complete record and
  the target/profile/features/compiler context match exactly.
- Any newer or older timestamp, touch, creation, or deletion invalidates the
  declaration record. Content changed while restoring the exact previous mtime,
  unrelated files/directories, environment changes, and changes to the
  `bake.build` wrapper do not invalidate this mtime record.
- This mtime rule is only for declaration reuse. Ordinary project sources and
  headers are read and SHA-256 checked for build input identity and ready-output
  reuse, so a source edit is detected even when its size and mtime are restored.
- `build.cpp` determines the input declaration. `bake.toml` remains authoritative
  for package identity, language/profile options, features, target settings,
  and link configuration.

## Write boundaries

- Project build products and Core state stay under `out/`; fetched sources and
  toolchain products stay under `~/.cache/bake/`.
- `build` and `compdb` write `compile_commands.json` at the workspace root (or
  the explicit `compdb` output path). `add`/`update` edit `bake.toml` and
  `bake.lock`. Standalone `cc`/`c++`/`ar` write only paths named by their own
  arguments.
- Never moves a locked dependency tag during `build`.
- Validates Core and analysis records before reuse; invalid analysis entries are
  ignored or safely replaced. Use `bake clean` when you intentionally want to
  discard the complete project output and state tree.
