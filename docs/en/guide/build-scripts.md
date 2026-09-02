# Build Scripts (`build.cpp`)

Default discovery covers the standard layout. When your inputs need logic — generated file lists, unusual trees, extra binaries — add a `build.cpp` to the package root. It is bake's equivalent of a Cargo build script, but inverted: instead of running arbitrary commands at build time, a build.cpp *describes inputs* declaratively, and bake's normal pipeline does the rest.

```cpp
// build.cpp
import bake;

int main() {
    bake::Builder b;

    b.sources({"src/*.cpp", "src/detail/*.cpp"});
    b.public_modules("public/*.cppm");
    b.public_headers("include");

    return b.build();
}
```

## The three input tiers

A package operates in exactly one of three tiers, chosen by what exists and what the script declares:

1. **Default discovery** — no `build.cpp`. Inputs come from the standard `src/` + `public/` layout (see [First Steps](../getting-started/first-steps.md)).
2. **Incremental** — a `build.cpp` that declares **no** main-moid inputs (no `sources`, no `public_modules`, no `public_headers`). The main moid still gets default discovery; the script contributes only the extras: binaries, tests, prebuilt libs.
3. **Custom** — a `build.cpp` that declares main-moid inputs. Discovery is fully replaced: exactly what you declare is compiled, nothing else.

Tier choice is data-driven: `sources.empty() && public_include_dirs.empty()` after running the script ⇒ incremental; otherwise custom.

## What build.cpp can and cannot do

**Can** — describe inputs:

- which source files and module interfaces the moid compiles, and which are public
- include directories (private and public)
- prebuilt libraries (`.a`/`.lib` paths) to link
- extra binaries — additional executables built from the package (see below)
- test registrations (see [Testing](testing.md))

**Cannot** — change configuration. Compiler flags, defines, system libraries, frameworks, optimization: these belong in `bake.toml` (`[profile.*]`, `[link]`, `[target.*]`, `[features]`). A build.cpp cannot rename the moid, change its type, or override the manifest. The build script owns *what* is built; the manifest owns *how*.

This split is deliberate: input discovery can be arbitrarily project-specific, while configuration stays declarative, diffable, and target-gated.

## How the script runs

`build.cpp` is compiled by bake itself, fresh per project (like a header), and executed with context in environment variables — `BAKE_SOURCE_DIR`, `BAKE_TARGET`, `BAKE_MOID_NAME`, … The result is a declaration JSON consumed by the build pipeline. Scripts are cached: bake only recompiles a `build.cpp` when it, `bake.toml`, or the `bake.build` module is newer than the cached declaration.

Note for editor users: clangd cannot resolve `import bake;` in these files — that diagnostic is expected noise. bake compiles the script itself with the right module path.

The full `Builder` API is documented in the [build.cpp API reference](../reference/builder-api.md).

## Extra binaries

A `lib`/`dylib` moid can ship companion executables — CLI tools, test harnesses — that link against the library:

```cpp
bake::Builder b;

b.public_modules("public/*.cppm");

b.binary("mytool")
     .sources("tools/main.cpp")
     .include_dirs("tools");

return b.build();
```

Each `b.binary()` becomes its own synthetic moid: it compiles its sources, links the main library (and everything the library depends on), inherits the merged configuration, and can `import` the main moid's public modules. The binary lands in `out/<triple>/bin/`.

Declaring binaries on an `executable` moid is an error — the design assumes a library plus its tools.

## The `import std;` fast path

`build.cpp` runs as C++23 with the standard library available via `import std;` — you can use `std::filesystem`, `std::string`, ranges, whatever you need for input logic without any dependency beyond bake itself.
