# build.cpp API (`bake::Builder`)

A `build.cpp` imports the `bake` module and drives a `bake::Builder`:

```cpp
import bake;

int main() {
    bake::Builder b;
    // … describe inputs …
    return b.build();
}
```

The script owns **inputs**; `bake.toml` owns configuration (flags, defines, libraries). Patterns are globs relative to the package root (`*`, `**`, `?`); non-glob paths must exist.

## Input methods

All input methods chain and accept a single pattern or a braced list.

```cpp
b.sources({"src/*.cpp", "gen/output.cpp"}); // private translation units
b.public_modules("public/*.cppm");          // exported module interfaces
b.include_dirs("third_party/include");      // private include path
b.public_headers("include");                // include path exposed to consumers
b.prebuilt_lib("vendor/libz.a");            // prebuilt archive to link
```

| Method | Effect |
|---|---|
| `sources(patterns)` | Compile as private sources. Non-public module interfaces belong here too (e.g. `src/*.cppm`). |
| `public_modules(patterns)` | Compile as public module interfaces — dependents can `import` them. |
| `include_dirs(dirs)` | Private include directories (this moid only). |
| `public_headers(dirs)` | Public include directories (propagate to consumers). |
| `prebuilt_lib(path)` | Link a prebuilt static archive into the moid. |

## Extra binaries

`b.binary(name)` returns a `BinaryBuilder` describing an additional executable linked against the moid's library and dependencies:

```cpp
b.binary("mytool")
    .sources("tools/main.cpp")
    .include_dirs("tools");
```

Binary sources can `import` the main moid's public modules and use its public headers. Binaries are allowed only when the moid type is `lib` or `dylib`.

## Tests

```cpp
b.add_test("suite-name", "binary-name").set_default();
```

Registers a named test backed by a binary (declared via `binary()`). `set_default()` marks the test `bake test` runs by default; a later call clears earlier defaults. See [Testing](../guide/testing.md).

## Context accessors

| Member | Value |
|---|---|
| `b.option_bool("name")` | Resolved `[options]` value (graph-OR'd) |
| `b.source_dir()` | Package root (absolute) |
| `b.build_dir()` | `out/` directory (absolute) |
| `b.target()` | Active target triple |
| `b.dep_src_dir("alias")` | Source directory of a resolved dependency (empty if unknown) — useful for globbing a dependency's headers |

## Execution model

- The script is compiled by bake as C++23 and cached; it recompiles only when `build.cpp`, `bake.toml`, or the `bake.build` module changes.
- Context arrives via environment variables (`BAKE_SOURCE_DIR`, `BAKE_TARGET`, `BAKE_OPTIONS`, `BAKE_DEPS`, …) — the constructor reads them; you normally never touch them.
- `b.build()` writes the declaration and returns 0 on success. Returning anything else fails the build with the script's stderr shown.
- Tier selection is data-driven: a script that declares no main-moid inputs leaves default discovery active (incremental tier); any `sources`/`public_modules`/`public_headers` call switches to full custom input. See [Build Scripts](../guide/build-scripts.md).
- Editor note: clangd will report `Module 'bake' not found` for `build.cpp` — expected; bake compiles the script itself.
