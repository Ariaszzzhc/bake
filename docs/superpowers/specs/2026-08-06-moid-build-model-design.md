# Bake Moid Build Model Design

## Goal

Replace the current package-wide, always-linked build model with a single Moid model that:

- treats one `bake.toml` as one schedulable Moid;
- supports `lib`, `executable`, and `dylib` outputs;
- makes convention mode and `build.cpp` produce the same persisted declaration;
- configures a validated two-level DAG before executing build work;
- propagates objects, modules, headers, and link requirements according to Moid type;
- removes the project-owned `core` dynamic library and produces one `bake` executable;
- keeps toolchain runtime sources and modules distributable and cacheable.

The implementation is intentionally a Cargo-like subset. Cargo separates packages, targets, and crate types; Bake keeps one Moid per manifest for now. Multiple products remain expressible as multiple workspace Moids instead of adding multi-target manifest semantics in this refactor.

## Terminology

### Workspace

A set of local Moids built under one output root. A workspace is not itself linkable and does not produce an artifact.

### Moid

The smallest dependency and scheduling unit. A Moid has:

- a canonical identity;
- a display name and version;
- one type;
- one effective option set;
- source groups and build settings;
- dependencies on other Moids;
- exported compile and link usage;
- an internal action DAG.

One `bake.toml` describes one Moid. A source distribution that contains multiple Moids uses a workspace.

### Moid declaration

The Configure-stage, toolchain-independent description of one Moid. It is persisted as JSON and is the only input from convention or `build.cpp` to planning.

### Build graph

The Configure output that contains the outer Moid DAG and each Moid's inner action DAG. It is persisted as JSON, read back, validated, and only then executed.

## Moid Types

Three types: `executable`, `lib`, `dylib`.

### `executable`

`executable` is the default when a Moid declaration omits `type`. An executable Moid has a terminal link action and produces `out/bin/<name>`. It consumes:

- its own objects;
- dependency `lib` archives and `dylib` artifacts;
- transitive link requirements;
- the selected toolchain runtime objects and system ABI libraries.

An executable exports no downstream build usage and cannot be used as a link dependency.

### `lib`

`lib` must be explicitly declared. If the Moid has object-producing sources, it has a terminal archive action and produces `out/lib/lib<name>.a`. If it is header-only or module-only without private objects, it has no archive action.

The archive contains only the Moid's own objects. It does not embed dependent archives.

A `lib` Moid exports downstream:

- its `.a` artifact when it has objects;
- PCM artifacts for modules under `public/`;
- its `public/` include directory when present;
- transitive link requirements (dependency archives, dylibs, system libraries, frameworks).

### `dylib`

`dylib` must be explicitly declared. It has a terminal link action and produces the platform dynamic-library artifact under `out/lib/`. It links its own objects with dependency archives and dylibs. Downstream consumers receive the dylib artifact, plus public PCM/include usage and transitive link requirements.

### Defaults and validation

- A convention Moid with no `type` is `executable`.
- A `build.cpp` Builder starts as `executable` and may explicitly select any of the three types.
- Every library form (`lib` or `dylib`) requires an explicit declaration.
- An unknown type string is a configuration error.
- An executable used through a normal dependency edge is a configuration error.

## Manifest Model

The long-term vocabulary is Moid, so `[moid]` replaces `[package]` during this pre-release refactor:

```toml
[moid]
name = "codec"
version = "0.1.0"
type = "lib"       # required for every library form
std = "c++23"

[dependencies]
base = { path = "../base", options = { simd = true } }
```

An executable uses the default when `type` is omitted:

```toml
[moid]
name = "app"
std = "c++23"
```

Writing `type = "executable"` explicitly is also valid. Workspace manifests retain `[workspace]`. Dependency aliases remain local edge names; they do not define canonical Moid identity.

Because the project is pre-release, `[package]` is removed rather than supported as a compatibility alias. Invalid or mixed old/new manifests fail with a diagnostic that names the file and field.

## `build.cpp` Contract

`bake.build.cppm` is source-distributed and depends only on `import std;`. A build script executes during Configure and emits exactly one declaration document through the Bake-provided output channel.

The API exposes the same three types:

```cpp
import std;
import bake.build;

int main() {
    bake::Builder b;
    b.lib("codec")
     .sources("src/**/*.cpp")
     .public_sources("public/**/*.cppm")
     .public_include_dir("public");
    return b.build();
}
```

`executable()`, `lib()`, and `dylib()` select the declared Moid type and name. If no selector is called, the Builder describes the manifest Moid as `executable`. Source groups use an option object for per-source flags, defines, include paths, language selection, and other compile settings. The removed per-target flag API and `compile_flag_for` are not restored.

The script receives source paths, build paths, effective options, dependency source roots, and the Bake executable location through a structured invocation context. User logs go to stderr; the declaration is written to a dedicated file descriptor or path supplied by Bake so stdout cannot corrupt the protocol.

A build script does not compile, archive, link, discover dependency artifacts, or mutate the graph. Its only responsibility is to declare the current Moid from its source tree and resolved options.

## Configure Stage

Configure has five ordered responsibilities.

### 1. Resolve the Moid closure

Bake loads workspace members, path dependencies, and locked remote dependencies into canonical source identities. It restores the existing lock semantics:

- ordinary builds resolve and update a missing or stale lock;
- `--locked` refuses a missing or stale lock;
- `--offline` performs no network access and validates cached source content;
- `--frozen` combines both restrictions;
- tags never move during ordinary locked builds;
- only update operations re-resolve movable references.

Incoming option constraints for each canonical Moid are merged before configuring it. Incompatible effective option sets for one source identity are errors rather than silently producing one arbitrary variant.

### 2. Materialize every declaration

For each Moid, Bake either:

- runs convention declaration logic; or
- compiles and runs its `build.cpp`.

Both paths write `out/.bake/<moid-id>.moid.json`. Bake then discards the in-memory declaration and reads the file through the same strict parser. There is no convention shortcut.

Each declaration contains canonical identity, source root, name/version/type, language standard, source groups, compile settings, link requirements, effective options, and dependency edges.

### 3. Scan and plan

Bake scans all C++ module sources with the exact language, defines, include paths, target, and per-source flags that compilation will use. Scan failures for module interface sources are fatal.

It then builds:

- the outer Moid dependency DAG;
- each Moid's inner action DAG;
- cross-Moid edges from imported public modules to their producing actions;
- terminal artifact and link-interface propagation;
- synthetic toolchain prerequisites for standard modules and runtime objects.

Cycles, duplicate logical module names, duplicate output paths, unresolved imports, executable dependencies, and missing producers fail Configure.

### 4. Persist and reload the graph

Bake writes `out/.bake/graph.json`, discards the in-memory graph, reads it back, and validates it. JSON is an actual phase boundary, not a diagnostic copy.

### 5. Execute

Only a successfully reloaded and validated graph enters Build.

## Declaration Format

The JSON declaration is the current format with no versioning field:

```json
{
  "id": "workspace:codec",
  "name": "codec",
  "version": "0.1.0",
  "type": "lib",
  "root": "/absolute/source/root",
  "std": "c++23",
  "options": { "simd": true },
  "sources": [
    {
      "pattern": "src/**/*.cpp",
      "visibility": "private",
      "flags": [],
      "defines": [],
      "include_dirs": []
    },
    {
      "pattern": "public/**/*.cppm",
      "visibility": "public",
      "flags": [],
      "defines": [],
      "include_dirs": []
    }
  ],
  "public_include_dirs": ["public"],
  "link": {
    "libraries": [],
    "frameworks": []
  },
  "dependencies": [
    {
      "alias": "base",
      "id": "workspace:base",
      "options": { "simd": true }
    }
  ]
}
```

The declaration describes inputs and intent. Discovered modules, concrete object paths, commands, producer IDs, and propagated exports belong only in `graph.json`.

## Graph Model

### Outer Moid DAG

Each node contains:

- `MoidId` and display metadata;
- validated dependency edges;
- `MoidType`;
- computed compile usage;
- computed link interface;
- exported artifacts;
- one inner Action DAG.

`MoidId` is canonical and source-aware. Workspace/path identities derive from canonical roots; locked remote identities include immutable resolved source identity. Display names are not keys.

Dependency aliases are preserved on edges so `build.cpp` can resolve dependency source roots. They never replace canonical IDs.

### Inner Action DAG

Actions include module scan where needed, module compilation, ordinary compilation, archive, and link. Every generated output has exactly one producer. Every generated input has an explicit producer dependency.

Terminal archive/link actions depend on all own object producers, including module interface objects. They also depend on terminal artifacts from non-lib dependencies. No edge is inferred from map iteration or artifact naming.

### Artifact and usage types

The graph uses typed data rather than one string list:

- `ObjectArtifact`
- `ModuleArtifact`
- `StaticArchive` (produced by `lib`)
- `SharedLibrary` (produced by `dylib`)
- `Executable`
- `IncludeUsage`
- `DefineUsage`
- `SystemLibraryRequirement`
- `FrameworkRequirement`

A link input references an artifact and its producer action. A system library or framework is a requirement, not a file accidentally inserted into an archive.

### Lowering and validation

The executor may lower all inner graphs into one global ready queue for efficiency, but the persisted graph retains Moid boundaries. Before execution Bake validates:

- unique Moid IDs, action IDs, module names, and output paths;
- valid dependency references;
- acyclic outer and inner graphs;
- one producer for each generated input;
- reachable producer dependencies for terminal products;
- complete artifact exports for every type.

A non-empty pending set with no ready action is an error, never success.

## Scheduling

Moid dependencies are gates: a consumer Moid cannot activate until its dependencies have completed successfully. Independent Moids activate concurrently.

Within each active Moid, ready actions execute concurrently subject to module and generated-input dependencies. All active Moids share one bounded worker pool, avoiding nested pools and oversubscription.

A failed action prevents dependent actions and Moids from starting. Bake reports the failed action, command, relevant source, and the shortest dependency context. It does not update fingerprints for failed or unexecuted actions.

## Output Layout

Project-local outputs are:

```text
out/
├── bin/
├── lib/
├── .obj/<moid-id>/
├── .bmi/<moid-id>/
└── .bake/
    ├── graph.json
    ├── fingerprints.json
    ├── <moid-id>.moid.json
    └── scripts/<moid-id>/
        └── build_app
```

`out/.bake/scripts/` holds only project-specific compiled build programs. Shared `bake.build` and standard-library artifacts do not live there.

Toolchain-content-addressed outputs live under:

```text
~/.cache/bake/<toolchain-content-key>/
├── std/std.pcm
├── std/std.compat.pcm
├── bake.build/bake.build.pcm
├── bake.build/bake.build.o
└── runtime/<target>/
    ├── libcxx/*.o
    ├── libcxxabi/*.o
    ├── libunwind/*.o       # only for targets that require it
    └── compiler-rt/*       # only for targets that require it
```

The key includes Bake/toolchain identity, target, compiler flags that affect ABI, and content hashes of distributed sources. Cache writes are atomic and protected against concurrent Bake processes.

## Runtime and Platform Policy

Bake is the compiler driver and linker driver. Build actions invoke the integrated Clang and LLD entry points; they never spawn a system `clang`, `ld`, or `ar`.

On macOS:

- pure C and C++ use vendored headers and Bake-built C++ runtime objects;
- libc++ and libc++abi are statically linked into final executables by default;
- libSystem is obtained from the selected macOS SDK when an SDK is available;
- frameworks use the selected SDK;
- SDK discovery may invoke `xcrun --show-sdk-path` but has no hard-coded fallback path;
- the SDK must not replace vendored libc++ headers or runtime objects;
- deployment target and SDK metadata must be consistent so LLD does not warn that SDK libraries are newer than the target minimum.

Toolchain policy is part of graph planning, not injected into an already-built graph by CLI code.

## Removing `core`

The final repository layout has no separate `core` package or project-owned dynamic library:

```text
bake/
├── bake.toml
├── build.cpp
└── src/
    ├── main.cpp
    ├── cli.cppm
    ├── util.cppm
    ├── project.cppm
    ├── compiler.cppm
    ├── engine.cppm
    ├── package.cppm
    └── compiler/...
lib/
├── bake/bake.build.cppm
├── libc/...
├── libcxx/...
├── libcxxabi/...
├── libunwind/...
└── compiler-rt/...
third_party/
├── nlohmann/...
└── tomlplusplus/...
```

CMake stage0 compiles these sources directly into one `bake` executable. It does not create `libbake.dylib`, install a project runtime library, set a project-library RPATH, or require `DYLD_LIBRARY_PATH` in tests.

Stage0 may use the host C++ runtime as a bootstrap implementation detail. Stage1, built by stage0 Bake, uses vendored libc++/libc++abi runtime objects and statically linked LLVM/Clang/LLD/zlib/zstd inputs; on macOS its only expected dynamic system dependency is libSystem and platform-provided components it explicitly uses.

`lib/bake/bake.build.cppm` is installed as source alongside Bake. It is compiled into the global content-addressed cache on demand and is not linked through a C ABI or project dynamic library.

Third-party nlohmann/json and toml++ remain in `third_party/` and are lib Moids. Their public module interfaces compile once and flow to Bake through ordinary Moid exports; they do not create empty or unnecessary archives.

## Incremental State

`fingerprints.json` records only successfully completed actions. A fingerprint includes:

- normalized argv, working directory, and relevant environment;
- input content/metadata identity;
- toolchain-content key;
- dependency artifact identities;
- expected outputs.

Missing generated inputs are errors or pending producer dependencies, never ignored. Missing outputs force rebuild. A dependency object or library change propagates through producer edges and relinks consumers.

## Diagnostics

Configure errors name the Moid, manifest/build script, and field or dependency edge. Build errors name the action and source/output involved.

Human output is grouped by semantic units:

```text
Configuring codec v1.2.0
Configuring app v0.1.0
Compiling standard library modules
  [1/2] std
  [2/2] std.compat
Compiling codec v1.2.0
  [1/4] module codec.api
  [2/4] src/codec.cpp
Compiling app v0.1.0
  [1/2] src/main.cpp
  [2/2] link app
```

Dependency modules are reported under their own Moid, not interleaved as synthetic `[dep]` actions inside a consumer.

## Test Strategy

The refactor is driven by end-to-end fixtures plus focused graph tests.

Required behavior tests:

1. Explicit `lib` with objects produces a `.a` archive; header-only `lib` produces no archive.
2. Explicit `lib` module dependency exports a PCM that a consumer imports.
3. Default and explicit `executable` Moids consume dependency archives/dylibs and run successfully.
4. Explicit `dylib` produces a dynamic library and downstream consumers link against it.
5. Executable-as-normal-dependency fails Configure.
6. Diamond `lib` dependency de-duplicates archives in the link line.
7. Transitive headers, modules, defines, system libraries, and frameworks propagate deterministically.
8. Reverse-lexicographic dependency names still produce all required action edges.
9. Module-only Moids cannot archive/link before module object production.
10. Moid, module, and action cycles fail Configure.
11. Missing generated inputs and stalled graphs fail.
12. Convention and equivalent `build.cpp` produce equivalent declarations.
13. Both `.moid.json` and `graph.json` are read back before use; malformed persisted data fails.
14. Dependency options are applied only to their target Moid; incompatible incoming options fail.
15. Lock, offline, frozen, cache-integrity, and remote transitive dependency tests retain their prior semantics.
16. Stage0 builds Stage1; Stage1 rebuilds the workspace.
17. `otool -L` on Stage1 shows no project `libbake`, libc++, or libc++abi dylib dependency.
18. A copied distribution directory builds C and C++ smoke projects without Homebrew or a system compiler/linker.

## Migration Order

1. Add failing tests for the three Moid types and artifact propagation.
2. Introduce strict `MoidType` and `[moid]` parsing; make `executable` the omitted-type default and require every library form to be explicit.
3. Strictly parse declaration JSON; force both declaration paths through write/read.
4. Implement typed artifacts, usage requirements, and the outer Moid DAG.
5. Implement correct inner action producer edges, validation, and scheduling.
6. Restore option, lock, cache, and remote dependency semantics on the new Moid closure.
7. Move standard modules, `bake.build`, and runtime objects into the global toolchain cache.
8. Move engine/compiler/package/project/util sources into `bake`, move `bake.build.cppm` into `lib/bake`, and remove `core`.
9. Change CMake and self-build declarations to produce one executable without a project dynamic library.
10. Remove obsolete C ABI, package-type, `.pkgs`, duplicate pipeline, and compatibility code.
11. Run complete bootstrap, self-build, dependency, packaging, and dynamic-link verification.

Each step leaves one authoritative path. Old and new graph models are not maintained in parallel.
