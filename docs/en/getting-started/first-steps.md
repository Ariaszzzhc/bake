# First Steps

## Create a project

```bash
bake init hello
cd hello
```

This scaffolds:

```
hello/
├── bake.toml
├── src/
│   └── main.cpp
├── public/
└── tests/
```

Useful flags:

- `--type <executable|lib|dylib>` — moid type (default `executable`)
- `--std <c11|c17|c23|c++17|c++20|c++23>` — language standard (default `c++20`)

A C standard (e.g. `--std c17`) scaffolds genuine C sources. A `lib` project gets `src/lib.cpp` plus a public header under `public/<name>/`.

## Build and run

```bash
bake build     # compile + link, in parallel, incremental
bake run       # build if needed, then run the executable
```

```
   Building hello v0.1.0
    Finished in 0.52s
Hello from hello!
```

`bake run` forwards trailing positional arguments to your program:

```bash
bake run some-file.txt
```

Arguments starting with `-` are parsed as bake's own options — there is no
`--` separator yet, so pass dashed flags to your program via a wrapper or
environment.

If the workspace contains more than one executable, bake asks you to pick with `-p <member>`.

## The default project layout

With no configuration beyond `bake.toml`, bake discovers inputs by convention:

| Directory | Discovered as |
|---|---|
| `src/**/*.{cpp,cc,cxx,c}` | private sources |
| `src/**/*.cppm` | private module interfaces |
| `public/**/*.cppm` | **public** module interfaces, exported to consumers |
| `public/` | public include directory (headers are yours to place) |

Module interfaces found under `public/` are compiled and exported to dependent packages; module interfaces under `src/` stay internal. Headers are not compiled — `public/` is simply added as an include path for consumers.

Extension sets are configurable via `[sources]` in `bake.toml` (see the [manifest reference](../reference/manifest.md)).

## Incremental builds

Re-running `bake build` is cheap: bake fingerprints every action (content-based) and only re-execulates what changed. Editing one source file recompiles that file and relinks. Switching profiles (`--release`) or targets (`-t`) builds into separate output directories and both trees remain valid.

## Where things go

```
out/
└── aarch64-apple-darwin/    # one directory per target triple
    ├── bin/                 # executables
    ├── lib/                 # .a / .dylib
    ├── .obj/                # object files
    ├── .bmi/                # module PCMs
    └── .bake/               # resolved declarations, fingerprints
```

`bake clean` removes `out/`. Full details in [Output and Cache Layout](../reference/output-and-cache.md).

## A taste of modules

Set the standard to C++20 or later in `bake.toml`:

```toml
[language]
cxx = "c++23"
```

Then just write modules — no flags, no `clang-scan-deps`, no module map files:

```cpp
// src/math.cppm
export module math;
export int triple(int x) { return x * 3; }
```

```cpp
// src/main.cpp
import std;
import math;

int main() {
    std::println("{}", triple(14));    // 42
}
```

`import std;` works out of the box: bake pre-builds `std.pcm` from its vendored libc++ and caches it.

## Next steps

- [Dependencies](../guide/dependencies.md)
- [Build Scripts](../guide/build-scripts.md) — when default discovery is not enough
- [Testing](../guide/testing.md)
