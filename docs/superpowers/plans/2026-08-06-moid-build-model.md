# Moid Build Model Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace Bake's package-wide linked-output model with four strict Moid types, one persisted Configure protocol, a validated two-level DAG, and a single self-hosted Bake executable without a project dynamic library.

**Architecture:** One `bake.toml` describes one Moid. Convention mode and `build.cpp` both persist and reload a versioned `MoidDeclaration`; planning turns declarations into an outer Moid DAG containing typed exports and inner action DAGs, then persists, reloads, validates, and executes that graph. `executable` is the omitted-type default; `lib`, `staticlib`, and `sharedlib` are always explicit.

**Tech Stack:** C++23 modules, Clang/LLVM/LLD, libc++, nlohmann/json module, toml++ module, CMake 3.30+, Ninja, custom C++17 end-to-end test runner.

## Global Constraints

- Continue from the current dirty worktree at `ebf1e67`; do not reset, revert, or discard existing edits.
- The only Moid type spellings are `executable`, `lib`, `staticlib`, and `sharedlib`.
- `executable` is the default when `type` is absent; every library type is explicit.
- Convention mode and `build.cpp` must persist the same JSON schema and then reload it through one parser.
- `graph.json` must be reloaded and validated before execution; it is not diagnostic-only output.
- Use integrated Clang and LLD entry points; never spawn a system compiler, linker, or archiver.
- Keep `import std;` and C++23 conventions from `AGENTS.md`.
- Keep changes minimal within each task and remove replaced paths rather than maintaining compatibility branches.
- Do not create project-local `.pkgs/`; remote sources remain in the content-addressed source cache.
- Do not make any Git commit or other Git mutation without explicit approval for that specific action.
- Baseline on 2026-08-06: `cmake --build build` passes; `ctest --test-dir build --output-on-failure` reports 20 passed and 8 failed.

---

## Final File Responsibilities

The first six tasks may edit files under `core/` while behavior stabilizes. Task 9 moves them to their final locations in one mechanical change.

- `bake/src/bake.moid.cppm`: `MoidType`, declaration model, strict declaration JSON codec, canonical IDs.
- `bake/src/bake.graph.cppm`: typed artifacts, usage requirements, outer/inner graph model, graph JSON codec, structural validation.
- `bake/src/bake.project.cppm`: `[moid]`, workspace, dependencies, options, lockfile and output layout models.
- `bake/src/bake.package.cppm`: remote resolution, immutable source cache, lock/cache validation.
- `bake/src/bake.compiler.cppm`: compile/link command generation, integrated toolchain and toolchain-cache policy.
- `bake/src/bake.engine.cppm`: convention declaration, module scan, planning, action scheduling, fingerprints.
- `bake/src/bake.util.cppm`: filesystem, process, hashing, glob and cache-lock primitives.
- `bake/src/cli.cppm`: CLI parsing and orchestration only; it must not patch an already-planned graph.
- `lib/bake/bake.build.cppm`: source-distributed Configure API with no Bake runtime dependency.
- `tests/test_runner.cpp`: end-to-end contract tests.
- `tests/projects/`: fixture manifests and sources.
- `CMakeLists.txt`: stage0 single-executable bootstrap.
- `bake/build.cpp`: stage1 single-executable declaration.

---

### Task 1: Establish the Four Strict Moid Types

**Files:**
- Modify: `core/src/bake.project.cppm`
- Modify: `core/src/bake.engine.cppm`
- Modify: `core/public/bake.build.cppm`
- Modify: `bake/src/cli.cppm`
- Modify: every tracked `bake.toml`
- Modify: `tests/test_runner.cpp`
- Create: `tests/projects/default_executable/bake.toml`
- Create: `tests/projects/default_executable/src/main.cpp`
- Create: `tests/projects/invalid_moid_type/bake.toml`
- Create: `tests/projects/invalid_moid_type/src/main.cpp`

**Interfaces:**
- Produces:

```cpp
export enum class MoidType {
    Executable,
    Lib,
    StaticLib,
    SharedLib,
};

export std::string_view moid_type_str(MoidType type);
export std::expected<MoidType, std::string>
parse_moid_type(std::string_view text);

export struct Moid {
    std::string name;
    std::string version = "0.1.0";
    MoidType type = MoidType::Executable;
    std::string std_version = "c++17";
};
```

- Removes: `PackageType`, `Package`, `package_type_str()`, `parse_package_type()`, and all type fallbacks based on `value_or(Executable)`.

- [ ] **Step 1: Add default-executable and invalid-type fixtures**

`tests/projects/default_executable/bake.toml`:

```toml
[moid]
name = "default-executable"
version = "0.1.0"
std = "c++23"
```

`tests/projects/default_executable/src/main.cpp`:

```cpp
int main() {
    return 0;
}
```

`tests/projects/invalid_moid_type/bake.toml`:

```toml
[moid]
name = "invalid-moid-type"
version = "0.1.0"
type = "static-lib"
std = "c++23"
```

`tests/projects/invalid_moid_type/src/main.cpp`:

```cpp
int main() {
    return 0;
}
```

- [ ] **Step 2: Register failing tests**

Add tests equivalent to:

```cpp
TestResult test_default_executable_type() {
    auto dir = make_temp_dir("default_executable");
    copy_fixture("default_executable", dir);
    auto result = run_bake("build", dir);
    CHECK(result.success(), result.stdout);
    CHECK(fs::exists(dir / "out/bin/default-executable"),
          "missing default executable output");
    return {};
}

TestResult test_invalid_moid_type() {
    auto dir = make_temp_dir("invalid_moid_type");
    copy_fixture("invalid_moid_type", dir);
    auto result = run_bake("build", dir);
    CHECK(!result.success(), "invalid Moid type unexpectedly succeeded");
    CHECK(result.stdout.find("unknown moid type 'static-lib'") != std::string::npos,
          "missing strict Moid type diagnostic: " + result.stdout);
    return {};
}
```

- [ ] **Step 3: Run the focused tests and confirm failure**

Run:

```bash
cmake --build build
./build/test_runner ./build/bake "default_executable_type"
./build/test_runner ./build/bake "invalid_moid_type"
```

Expected: the first fixture is rejected because `[moid]` is not parsed; the second does not produce the required strict diagnostic.

- [ ] **Step 4: Rename the manifest model and parse `[moid]` strictly**

Implement the interfaces above. Parse absence of `type` as `MoidType::Executable`. When a type key is present, return a manifest-load error containing the invalid token instead of retaining a default value.

- [ ] **Step 5: Migrate all manifests and build API type strings**

Apply these exact token migrations:

```text
[package]            -> [moid]
static-lib           -> staticlib
shared-lib           -> sharedlib
```

Keep executable manifests explicit where they already are. Mark nlohmann/json and toml++ as `type = "lib"`; do not let them inherit executable.

- [ ] **Step 6: Update CLI init/run and declaration defaults**

`bake init` may emit `type = "executable"` explicitly. `bake run` must inspect the configured Moid type rather than the removed package type. `MoidDeclaration` and Builder defaults become executable; unknown JSON types return an error.

- [ ] **Step 7: Run focused and full tests**

Run:

```bash
cmake --build build
./build/test_runner ./build/bake "default_executable_type"
./build/test_runner ./build/bake "invalid_moid_type"
ctest --test-dir build --output-on-failure
```

Expected: both new tests pass. Existing failures may remain only in the previously recorded lock/options/remote set.

- [ ] **Step 8: Review checkpoint**

Run `git diff --check`. Inspect the diff for any `PackageType`, `[package]`, `static-lib`, or `shared-lib` references outside historical design documents. Do not commit without explicit approval.

---

### Task 2: Make Declaration JSON the Single Configure Boundary

**Files:**
- Create: `core/src/bake.moid.cppm`
- Modify: `CMakeLists.txt`
- Modify: `core/build.cpp`
- Modify: `core/src/bake.engine.cppm`
- Modify: `core/public/bake.build.cppm`
- Modify: `bake/src/cli.cppm`
- Modify: `tests/test_runner.cpp`
- Create: `tests/projects/declaration_equivalence/convention/bake.toml`
- Create: `tests/projects/declaration_equivalence/convention/src/main.cpp`
- Create: `tests/projects/declaration_equivalence/scripted/bake.toml`
- Create: `tests/projects/declaration_equivalence/scripted/build.cpp`
- Create: `tests/projects/declaration_equivalence/scripted/src/main.cpp`
- Create: `tests/projects/invalid_declaration_schema/bake.toml`
- Create: `tests/projects/invalid_declaration_schema/build.cpp`

**Interfaces:**
- Produces:

```cpp
export inline constexpr std::uint32_t moid_declaration_schema = 1;

export struct SourceOptions {
    std::vector<std::string> flags;
    std::vector<std::string> defines;
    std::vector<std::string> include_dirs;
};

export struct SourceGroup {
    std::string pattern;
    bool is_public = false;
    SourceOptions options;
};

export struct MoidDependency {
    std::string alias;
    std::string id;
    std::map<std::string, BuildOption> options;
};

export struct MoidDeclaration {
    std::uint32_t schema = moid_declaration_schema;
    std::string id;
    std::string name;
    std::string version;
    MoidType type = MoidType::Executable;
    std::string root;
    std::string std_version;
    std::map<std::string, BuildOption> options;
    std::vector<SourceGroup> sources;
    std::vector<std::string> public_include_dirs;
    std::vector<std::string> libraries;
    std::vector<std::string> frameworks;
    std::vector<MoidDependency> dependencies;
};

export std::expected<MoidDeclaration, std::string>
read_moid_declaration(const Path& path);

export std::expected<void, std::string>
write_moid_declaration(const Path& path, const MoidDeclaration& declaration);
```

- Consumes: `MoidType` and `BuildOption` from Task 1.

- [ ] **Step 1: Add equivalent convention and scripted fixtures**

Both fixtures declare `declaration-equivalence` as the default executable with `src/main.cpp`. The scripted `build.cpp` is:

```cpp
import bake.build;
import std;

int main() {
    bake::Builder builder;
    builder.sources("src/main.cpp").std("c++23");
    return builder.build();
}
```

- [ ] **Step 2: Add declaration protocol tests**

Build each fixture, parse `out/.bake/*.moid.json` with the test runner's text utilities, and assert both contain:

```json
{"schema":1,"name":"declaration-equivalence","type":"executable"}
```

Compare normalized semantic fields rather than absolute `root` paths. The `invalid_declaration_schema` fixture uses this build script to exercise the normal read path without adding a test-only CLI command:

```cpp
import std;

int main() {
    const char* path = std::getenv("BAKE_DECLARATION_PATH");
    if (path == nullptr) return 2;
    std::ofstream output(path);
    output << R"({"schema":999})";
    return output ? 0 : 3;
}
```

Its test runs `bake build` and requires `unsupported moid declaration schema 999`.

- [ ] **Step 3: Run the tests and confirm failure**

Run:

```bash
cmake --build build
./build/test_runner ./build/bake "declaration_equivalence"
./build/test_runner ./build/bake "declaration_schema"
```

Expected: the current convention path does not reload JSON, and schema validation is absent.

- [ ] **Step 4: Move declaration ownership into `bake.moid`**

Move `MoidDeclaration` and its nlohmann/json codec out of `bake.engine`. Make parsing strict for schema, required fields, field types, duplicate dependencies, and unknown Moid type values. A missing `type` is normalized to executable.

- [ ] **Step 5: Change `bake.build` to the exact four selectors**

Expose these signatures:

```cpp
Builder& executable(std::string name);
Builder& lib(std::string name);
Builder& staticlib(std::string name);
Builder& sharedlib(std::string name);
```

Initialize Builder from `BAKE_MOID_NAME` and `BAKE_MOID_VERSION`, with type `executable`. `build()` writes JSON to the path in `BAKE_DECLARATION_PATH`. It returns nonzero if the path or name is unavailable. User output remains free to use stdout/stderr because JSON no longer uses stdout.

- [ ] **Step 6: Force both declaration paths through disk**

For convention mode:

```cpp
auto declaration = convention_declare(context);
auto path = declaration_path(context, declaration.id);
write_moid_declaration(path, declaration);
declaration = read_moid_declaration(path).value();
```

For `build.cpp`, set `BAKE_DECLARATION_PATH`, run the program, then call only `read_moid_declaration(path)`. Remove stdout JSON capture and remove every call path that passes an in-memory convention declaration directly to planning.

- [ ] **Step 7: Build and run protocol tests**

Run:

```bash
cmake --build build
./build/test_runner ./build/bake "declaration_equivalence"
./build/test_runner ./build/bake "declaration_schema"
ctest --test-dir build --output-on-failure
```

Expected: declaration tests pass and no existing passing test regresses.

- [ ] **Step 8: Review checkpoint**

Search for `moid_decl_from_json_string`, stdout declaration parsing, and direct convention-to-graph calls. Remove them. Run `git diff --check`; do not commit without explicit approval.

---

### Task 3: Build the Canonical Outer Moid DAG and Effective Options

**Files:**
- Create: `core/src/bake.graph.cppm`
- Modify: `CMakeLists.txt`
- Modify: `core/build.cpp`
- Modify: `core/src/bake.project.cppm`
- Modify: `core/src/bake.engine.cppm`
- Modify: `bake/src/cli.cppm`
- Modify: `tests/test_runner.cpp`
- Modify: `tests/projects/build_cpp_meta_dep/**`
- Create: `tests/projects/executable_dependency/bake.toml`
- Create: `tests/projects/executable_dependency/app/bake.toml`
- Create: `tests/projects/executable_dependency/app/src/main.cpp`
- Create: `tests/projects/executable_dependency/tool/bake.toml`
- Create: `tests/projects/executable_dependency/tool/src/main.cpp`

**Interfaces:**
- Produces:

```cpp
export struct MoidId {
    std::string value;
    auto operator<=>(const MoidId&) const = default;
};

export struct MoidEdge {
    std::string alias;
    MoidId target;
    std::map<std::string, BuildOption> options;
};

export struct MoidNode {
    MoidId id;
    MoidDeclaration declaration;
    std::vector<MoidEdge> dependencies;
};

export struct MoidGraph {
    std::map<MoidId, MoidNode> nodes;
    std::vector<MoidId> roots;
};

export struct BuildSelection {
    std::optional<std::string> workspace_member;
    std::map<std::string, BuildOption> root_options;
};

export struct ResolvePolicy {
    bool locked = false;
    bool offline = false;
};

export std::expected<MoidGraph, std::string>
resolve_moid_graph(const Manifest& root,
                   const BuildSelection& selection,
                   const ResolvePolicy& policy);

export std::expected<std::vector<MoidId>, std::string>
topological_moids(const MoidGraph& graph);
```

- Removes: name-only `seen_names`, name-only `depends_on`, root CLI options copied to every dependency, and `continue` after dependency configuration failure.

- [ ] **Step 1: Add an executable-dependency rejection test**

The root fixture depends normally on `tool`, whose manifest omits type and is therefore executable. Assert Configure fails with:

```text
moid 'app' cannot use executable moid 'tool' as a normal dependency
```

- [ ] **Step 2: Strengthen effective-option tests**

Keep `convention_meta_dependency` as the primary test. Add an assertion that root CLI options are not visible in `answer` unless passed on the `answer` dependency edge. Keep the existing conflicting workspace configuration test and require a diagnostic that names both incoming aliases.

- [ ] **Step 3: Run focused tests and confirm failure**

Run:

```bash
cmake --build build
./build/test_runner ./build/bake "executable_dependency"
./build/test_runner ./build/bake "convention_meta_dependency"
./build/test_runner ./build/bake "workspace_unified_output"
```

Expected: executable dependencies are currently accepted or linked, dependency option `bias` is lost, and duplicate configurations are not rejected correctly.

- [ ] **Step 4: Implement canonical identities**

Use these identity forms:

```text
workspace:<canonical-member-relative-path>
path:<sha256-of-canonical-absolute-root>
git:<normalized-url>#<resolved-commit>:<tree-sha256>
```

Names remain display metadata. Resolve aliases to IDs before declaration generation. Preserve alias and effective options on every edge.

- [ ] **Step 5: Merge incoming options before Configure**

Collect all incoming edge option maps for one canonical ID. Validate option names and types against the target manifest. If two edges request different values for one option, fail before compiling `build.cpp`. Pass only the resulting target-specific map through `BAKE_OPTIONS`.

- [ ] **Step 6: Validate and topologically sort the outer graph**

Reject missing targets, duplicate IDs, executable dependency edges, and cycles. Diagnostics must include an alias path, for example:

```text
moid dependency cycle: app -> codec -> base -> app
```

- [ ] **Step 7: Persist declaration dependencies by canonical ID**

Populate `MoidDeclaration::dependencies` from resolved edges for both convention and scripted Moids. Remove path-dependency and lockfile-dependency collection branches that append declarations separately after the graph already exists.

- [ ] **Step 8: Run focused and full tests**

Run:

```bash
cmake --build build
./build/test_runner ./build/bake "executable_dependency"
./build/test_runner ./build/bake "convention_meta_dependency"
./build/test_runner ./build/bake "workspace_unified_output"
ctest --test-dir build --output-on-failure
```

Expected: all three focused tests pass. No configuration failure is converted into a partial successful graph.

- [ ] **Step 9: Review checkpoint**

Search for `seen_names`, dependency lookup by display name, and root options passed to dependency Configure. Remove each old branch. Run `git diff --check`; do not commit without explicit approval.

---

### Task 4: Implement Typed Exports for `lib`, `staticlib`, `sharedlib`, and `executable`

**Files:**
- Modify: `core/src/bake.graph.cppm`
- Modify: `core/src/bake.engine.cppm`
- Modify: `core/src/bake.compiler.cppm`
- Modify: `tests/test_runner.cpp`
- Create: `tests/projects/moid_outputs/bake.toml`
- Create: `tests/projects/moid_outputs/base/bake.toml`
- Create: `tests/projects/moid_outputs/base/public/base.hpp`
- Create: `tests/projects/moid_outputs/base/src/base.cpp`
- Create: `tests/projects/moid_outputs/archive/bake.toml`
- Create: `tests/projects/moid_outputs/archive/public/archive.hpp`
- Create: `tests/projects/moid_outputs/archive/src/archive.cpp`
- Create: `tests/projects/moid_outputs/shared/bake.toml`
- Create: `tests/projects/moid_outputs/shared/public/shared.hpp`
- Create: `tests/projects/moid_outputs/shared/src/shared.cpp`
- Create: `tests/projects/moid_outputs/app/bake.toml`
- Create: `tests/projects/moid_outputs/app/src/main.cpp`

**Interfaces:**
- Produces:

```cpp
export enum class ArtifactKind {
    Object,
    Module,
    StaticLibrary,
    SharedLibrary,
    Executable,
};

export struct ArtifactRef {
    ArtifactKind kind;
    Path path;
    std::string producer_action;
    auto operator<=>(const ArtifactRef&) const = default;
};

export struct CompileUsage {
    std::vector<Path> include_dirs;
    std::vector<std::string> defines;
    std::map<std::string, ArtifactRef> modules;
};

export struct LinkInterface {
    std::vector<ArtifactRef> objects;
    std::vector<ArtifactRef> libraries;
    std::vector<std::string> system_libraries;
    std::vector<std::string> frameworks;
};

export struct MoidExports {
    CompileUsage compile;
    LinkInterface link;
    std::optional<ArtifactRef> terminal;
};
```

- [ ] **Step 1: Create the four-output fixture**

Use a workspace with:

```toml
[workspace]
members = ["base", "archive", "shared", "app"]
```

`base` is `type = "lib"`; `archive` is `type = "staticlib"` and depends on base; `shared` is `type = "sharedlib"`; `app` is default executable and depends on archive and shared. `main.cpp` verifies symbols from all three library Moids.

- [ ] **Step 2: Add output and execution assertions**

Assert:

```text
out/.obj/base/ contains base.o
out/lib/ contains no artifact named for base
out/lib/libarchive.a exists
out/lib/libshared.dylib or platform equivalent exists
out/bin/app exists and exits 0
```

Read `out/.bake/graph.json` and assert the `archive` action lists the `base.o` artifact exactly once and lists no system library or framework as an archive member. Running `app` proves the archive contains the required code without adding an external archive-tool dependency to the test.

- [ ] **Step 3: Run the fixture and confirm failure**

Run:

```bash
cmake --build build
./build/test_runner ./build/bake "moid_outputs"
```

Expected: `lib` is not planned, every dependency currently expects a terminal output, or link/archive inputs are wrong.

- [ ] **Step 4: Compute compile usage topologically**

For each node, merge dependency public include directories, defines, and modules in outer topological order. Reject duplicate logical module names unless both references identify the same artifact.

- [ ] **Step 5: Implement exact type export rules**

Use these rules without special cases based on names:

```text
lib        terminal = none; objects = own objects + transitive lib objects
staticlib  archive inputs = own objects + transitive lib objects
           export terminal archive; export no objects
sharedlib  link inputs = own objects + transitive lib objects + dependency libraries
           export terminal shared library; export no objects
executable link inputs = own objects + transitive lib objects + dependency libraries
           export no downstream usage
```

De-duplicate by `(ArtifactKind, normalized path, producer_action)`, preserving topological link order.

- [ ] **Step 6: Separate archive members from link requirements**

`ArchiveCommand` accepts object paths only. System libraries, frameworks, static archives, and shared libraries are never archive members. `LinkCommand` receives file artifacts once and system/framework requirements once.

- [ ] **Step 7: Run output and regression tests**

Run:

```bash
cmake --build build
./build/test_runner ./build/bake "moid_outputs"
./build/test_runner ./build/bake "static_lib_build"
./build/test_runner ./build/bake "path_dep_build"
ctest --test-dir build --output-on-failure
```

Expected: the four-output test passes and link commands do not contain duplicated zlib/zstd/LLVM entries.

- [ ] **Step 8: Review checkpoint**

Inspect `out/.bake/graph.json` and verify `base` has no terminal action, archive members are objects only, and app consumes archive/shared artifacts. Run `git diff --check`; do not commit without explicit approval.

---

### Task 5: Make Module and Action DAG Edges Complete and Validated

**Files:**
- Modify: `core/src/bake.graph.cppm`
- Modify: `core/src/bake.engine.cppm`
- Modify: `tests/test_runner.cpp`
- Create: `tests/projects/reverse_dependency_order/**`
- Create: `tests/projects/module_only_lib/**`
- Create: `tests/projects/duplicate_module/**`
- Create: `tests/projects/moid_cycle/**`

**Interfaces:**
- Produces:

```cpp
export struct BuildAction {
    std::string id;
    std::string moid_id;
    std::string kind;
    std::vector<std::string> dependencies;
    std::vector<Path> inputs;
    std::vector<Path> outputs;
    std::vector<std::string> argv;
    Path working_directory;
};

export struct ActionGraph {
    std::map<std::string, BuildAction> actions;
};

export struct PlannedMoid {
    MoidNode node;
    MoidExports exports;
    ActionGraph actions;
};

export struct BuildGraph {
    std::uint32_t schema = 1;
    std::map<MoidId, PlannedMoid> moids;
    std::vector<MoidId> roots;
};

export std::expected<void, std::string>
write_build_graph(const Path& path, const BuildGraph& graph);
export std::expected<BuildGraph, std::string>
read_build_graph(const Path& path);
export std::expected<void, std::string>
validate_build_graph(const BuildGraph& graph);
```

- [ ] **Step 1: Add adversarial graph fixtures**

Create:

- `aaa-consumer -> zzz-lib` to make map-order edge omissions deterministic;
- a `lib` containing only a public module interface and a consumer that imports it;
- two dependencies exporting the same logical module name;
- a three-node Moid dependency cycle.

- [ ] **Step 2: Add failing assertions**

Run clean builds with `--jobs 8` three times for the reverse-order and module-only fixtures. Duplicate modules must fail with both source paths. The cycle must fail during Configure and must not write fingerprints.

- [ ] **Step 3: Confirm current failures**

Run:

```bash
cmake --build build
./build/test_runner ./build/bake "reverse_dependency_order"
./build/test_runner ./build/bake "module_only_lib"
./build/test_runner ./build/bake "duplicate_module"
./build/test_runner ./build/bake "moid_cycle"
```

Expected: missing link/module producer edges or false-success cycle behavior appears.

- [ ] **Step 4: Build action producers before consumers**

Create all action records and output-to-producer indexes first. In a second pass, add dependencies for:

- imported PCMs;
- every object consumed by archive/link;
- every dependency terminal artifact;
- generated runtime/toolchain artifacts.

Never add an edge conditionally based on whether an earlier map iteration already created the producer.

- [ ] **Step 5: Scan modules with compile-equivalent settings**

Pass target, standard, defines, include directories, language mode, module search paths, and per-source flags to `clang-scan-deps`. A module interface scan failure is fatal. Reject duplicate provided module names and module cycles.

- [ ] **Step 6: Implement graph validation and JSON round-trip**

Validate unique IDs/outputs, valid dependency IDs, acyclicity, and generated-input producer reachability. Write `graph.json`, destroy the in-memory graph, read it back, call `validate_build_graph()`, and pass only the reloaded value to execution.

- [ ] **Step 7: Run adversarial and full tests**

Run:

```bash
cmake --build build
./build/test_runner ./build/bake "reverse_dependency_order"
./build/test_runner ./build/bake "module_only_lib"
./build/test_runner ./build/bake "duplicate_module"
./build/test_runner ./build/bake "moid_cycle"
ctest --test-dir build --output-on-failure
```

Expected: all adversarial fixtures are deterministic and malformed graphs fail before action execution.

- [ ] **Step 8: Review checkpoint**

Search for topo-sort fallback that appends cycle members, missing-input `continue`, and `ready.empty()` success exits. Remove them. Run `git diff --check`; do not commit without explicit approval.

---

### Task 6: Replace Wave Execution with One Bounded Two-Level Scheduler

**Files:**
- Modify: `core/src/bake.graph.cppm`
- Modify: `core/src/bake.engine.cppm`
- Modify: `bake/src/cli.cppm`
- Modify: `tests/test_runner.cpp`
- Create: `tests/projects/parallel_moids/**`
- Create: `tests/projects/incremental_relink/**`

**Interfaces:**
- Produces:

```cpp
export struct ExecuteOptions {
    std::size_t jobs = 1;
    bool verbose = false;
};

export struct ExecuteResult {
    std::size_t executed = 0;
    std::size_t cached = 0;
};

export struct ActionFingerprint {
    std::string digest;
    std::vector<Path> outputs;
};

export class FingerprintStore {
public:
    static std::expected<FingerprintStore, std::string> load(const Path& path);
    bool matches(std::string_view action_id,
                 const ActionFingerprint& fingerprint) const;
    void record(std::string action_id, ActionFingerprint fingerprint);
    std::expected<void, std::string> save(const Path& path) const;
private:
    std::map<std::string, ActionFingerprint> entries_;
};

export std::expected<ExecuteResult, std::string>
execute_graph(const BuildGraph& graph,
              const ExecuteOptions& options,
              FingerprintStore& fingerprints);
```

- [ ] **Step 1: Add parallel and relink tests**

The parallel fixture has two independent `lib` Moids consumed by one executable. Assert output groups actions under each Moid and the final executable runs. The relink fixture builds, changes a dependency source return value, rebuilds, and verifies the executable observes the new value.

- [ ] **Step 2: Confirm the relink failure or nondeterminism**

Run:

```bash
cmake --build build
./build/test_runner ./build/bake "parallel_moids"
./build/test_runner ./build/bake "incremental_relink"
```

Expected: the current wave scheduler may leave stale links or fail to represent Moid activation cleanly.

- [ ] **Step 3: Implement a global ready queue**

Maintain one indegree table for actions and one remaining-dependency count for Moids. Activate a Moid only after all dependency Moids finish. Push its zero-indegree actions into a shared ready queue. Use exactly `jobs` workers across all active Moids.

- [ ] **Step 4: Implement failure propagation**

On first action failure, stop scheduling dependent work, allow running actions to finish, and return a diagnostic with action ID, Moid name, source/output, and command. Never update fingerprints for failed or unexecuted actions.

- [ ] **Step 5: Make fingerprints producer-aware**

Fingerprint normalized argv, cwd, relevant environment, toolchain key, input identities, dependency artifact identities, and expected outputs. Missing output forces rebuild. Missing generated input is a graph error, not a cached action.

- [ ] **Step 6: Run repeated parallel and incremental tests**

Run:

```bash
cmake --build build
for i in 1 2 3; do ./build/test_runner ./build/bake "parallel_moids" || exit 1; done
./build/test_runner ./build/bake "incremental_relink"
ctest --test-dir build --output-on-failure
```

Expected: deterministic success and correct relinking.

- [ ] **Step 7: Review checkpoint**

Confirm no nested thread pool exists and `--jobs` bounds total workers. Run `git diff --check`; do not commit without explicit approval.

---

### Task 7: Restore Lock, Remote Source, and Offline Semantics on the Moid Graph

**Files:**
- Modify: `core/src/bake.project.cppm`
- Modify: `core/src/bake.package.cppm`
- Modify: `bake/src/cli.cppm`
- Modify: `tests/test_runner.cpp`
- Modify: remote-dependency fixtures generated by `tests/test_runner.cpp`

**Interfaces:**
- Consumes: `ResolvePolicy` and `MoidId` from Task 3.
- Produces:

```cpp
export struct ResolvedSource {
    MoidId id;
    Path root;
    std::string resolved_commit;
    std::string tree_sha256;
};

export std::expected<ResolvedSource, std::string>
materialize_locked_source(const LockNode& node,
                          const ResolvePolicy& policy,
                          const Path& cache_root);
```

- [ ] **Step 1: Re-run and preserve the eight failing baseline tests**

Run:

```bash
./build/test_runner ./build/bake "frozen_no_lock"
./build/test_runner ./build/bake "lock_consistency"
./build/test_runner ./build/bake "lock_transitive_consistency"
./build/test_runner ./build/bake "frozen_missing_cache"
./build/test_runner ./build/bake "workspace_unified_output"
./build/test_runner ./build/bake "convention_meta_dependency"
./build/test_runner ./build/bake "build_cpp_options"
./build/test_runner ./build/bake "remote_archive_extract"
```

Expected: failures match the recorded baseline until this task is implemented.

- [ ] **Step 2: Recover semantics, not the old build pipeline**

Read the former implementation with:

```bash
git show ebf1e67:bake/src/cli.cppm
```

Port only resolver, lock-staleness, offline, cache-integrity, and redownload decisions into the new pre-Configure Moid resolution phase. Do not restore old convention/build.cpp execution functions.

- [ ] **Step 3: Use immutable cache paths correctly**

Resolve native remote source roots as:

```cpp
Path source_root = cache_root / lock_node.tree_sha256;
```

Traverse every `LockNode::dependencies` edge, preserve parent aliases, and configure the complete transitive native Moid closure. Non-native source dependencies are represented as explicit source inputs owned by a declared Moid; they are not silently skipped.

- [ ] **Step 4: Restore exact policy behavior**

Implement:

```text
normal build  -> resolve missing/stale lock; fetch missing cache
--locked      -> fail on missing/stale lock; may fetch locked source
--offline     -> never use network; use and validate existing lock/cache
--frozen      -> --locked + --offline
update        -> may move tags and rewrite immutable commits
```

A cache tree hash mismatch is treated as missing/corrupt. Offline fails; online mode redownloads the locked immutable source.

- [ ] **Step 5: Remove `.pkgs` expectations**

Update remote tests to assert source-cache use and Moid graph entries instead of project-local `.pkgs/package.json`. Assert no `out/.pkgs` or project `.pkgs` directory is created.

- [ ] **Step 6: Run all dependency-policy tests**

Run:

```bash
cmake --build build
./build/test_runner ./build/bake "frozen_no_lock"
./build/test_runner ./build/bake "lock_consistency"
./build/test_runner ./build/bake "lock_transitive_consistency"
./build/test_runner ./build/bake "frozen_missing_cache"
./build/test_runner ./build/bake "workspace_unified_output"
./build/test_runner ./build/bake "convention_meta_dependency"
./build/test_runner ./build/bake "build_cpp_options"
./build/test_runner ./build/bake "remote_archive_extract"
ctest --test-dir build --output-on-failure
```

Expected: all 28 legacy tests plus new tests pass.

- [ ] **Step 7: Review checkpoint**

Search for `cache_dir / node_id`, root-only lock traversal, `.pkgs`, and ordinary-build lock bypasses. Remove each. Run `git diff --check`; do not commit without explicit approval.

---

### Task 8: Move Toolchain Modules and Runtime Objects to the Global Cache

**Files:**
- Modify: `core/src/bake.util.cppm`
- Modify: `core/src/bake.compiler.cppm`
- Modify: `core/src/bake.engine.cppm`
- Modify: `bake/src/cli.cppm`
- Modify: `tests/test_runner.cpp`
- Modify: `core/public/bake.build.cppm`

**Interfaces:**
- Produces:

```cpp
export struct ToolchainCache {
    Path root;
    std::string content_key;

    Path std_pcm() const;
    Path std_compat_pcm() const;
    Path bake_build_pcm() const;
    Path bake_build_object() const;
    Path runtime_dir(std::string_view target) const;
};

export std::expected<ToolchainCache, std::string>
open_toolchain_cache(const Toolchain& toolchain,
                     std::string_view target,
                     const Path& distributed_lib_dir);
```

- [ ] **Step 1: Add cache isolation to the test harness**

Make `run_bake()` accept `BAKE_CACHE_DIR=<temp>/cache`. Add a test that builds two copied projects against the same cache and asserts:

```text
cache contains std.pcm and std.compat.pcm
cache contains bake.build.pcm and bake.build.o after scripted build
neither project out/ contains .bmi/.std
neither project out/.bake/scripts contains bake.build.pcm or bake.build.o
```

- [ ] **Step 2: Run the cache test and confirm failure**

Run:

```bash
cmake --build build
./build/test_runner ./build/bake "global_toolchain_cache"
```

Expected: standard/wrapper artifacts currently remain project-local.

- [ ] **Step 3: Compute one content key**

Hash compiler identity, target, ABI-affecting flags, Bake version, LLVM revision, and contents of distributed `std.cppm`, `std.compat.cppm`, `bake.build.cppm`, libc++, libc++abi, and selected runtime source manifests. Do not include absolute install paths.

- [ ] **Step 4: Add atomic cross-process cache writes**

Build into a sibling temporary path, fsync/close outputs, and atomically rename them under the content-key directory. Guard each cache entry with a target-local lock file. A second process rechecks outputs after acquiring the lock.

- [ ] **Step 5: Relocate standard modules, wrapper, and runtime objects**

Make graph actions reference cache artifacts by typed `ArtifactRef`. Keep project-specific `build_app` under `out/.bake/scripts/<moid-id>/`; remove project-local wrapper PCM/object and `.bmi/.std` paths.

- [ ] **Step 6: Verify cache reuse**

Run:

```bash
cmake --build build
./build/test_runner ./build/bake "global_toolchain_cache"
ctest --test-dir build --output-on-failure
```

Expected: the second project reports cache hits and all tests pass.

- [ ] **Step 7: Review checkpoint**

Verify the content key contains no absolute checkout path and project cleanup never deletes the global cache. Run `git diff --check`; do not commit without explicit approval.

---

### Task 9: Remove `core` and Build One Bake Executable

**Files:**
- Move: `core/src/bake.util.cppm` -> `bake/src/bake.util.cppm`
- Move: `core/src/bake.project.cppm` -> `bake/src/bake.project.cppm`
- Move: `core/src/bake.moid.cppm` -> `bake/src/bake.moid.cppm`
- Move: `core/src/bake.graph.cppm` -> `bake/src/bake.graph.cppm`
- Move: `core/src/bake.compiler.cppm` -> `bake/src/bake.compiler.cppm`
- Move: `core/src/bake.engine.cppm` -> `bake/src/bake.engine.cppm`
- Move: `core/src/bake.package.cppm` -> `bake/src/bake.package.cppm`
- Move: `core/src/compiler/` -> `bake/src/compiler/`
- Move: `core/public/bake.build.cppm` -> `lib/bake/bake.build.cppm`
- Create: `bake/build.cpp`
- Modify: `bake/bake.toml`
- Modify: root `bake.toml`
- Modify: `CMakeLists.txt`
- Modify: `tests/test_runner.cpp`
- Delete: `core/build.cpp`
- Delete: `core/bake.toml`
- Delete: empty `core/` directories

**Interfaces:**
- Consumes: stable modules and graph behavior from Tasks 1–8.
- Produces: one CMake target named `bake` and one stage1 Moid named `bake`.

- [ ] **Step 1: Add a no-project-dylib test**

After CMake build, run `otool -L build/bake` on macOS or `ldd build/bake` on Linux. Assert no dependency path contains `libbake`, and remove the test harness's automatic `DYLD_LIBRARY_PATH` injection.

- [ ] **Step 2: Move source files without changing module names**

Use the exact move list above. Update imports only where a physical-path assumption exists. Remove deleted C ABI references rather than adding stubs.

- [ ] **Step 3: Replace CMake `core` with direct executable sources**

The stage0 shape becomes:

```cmake
add_executable(bake
    bake/src/main.cpp
    bake/src/compiler/bake_llvm.cpp
    bake/src/compiler/bake_clang_driver.cpp
    bake/src/compiler/bake_clang_cc1_main.cpp
)

target_sources(bake PUBLIC FILE_SET CXX_MODULES FILES
    third_party/nlohmann/public/nlohmann.json.cppm
    third_party/tomlplusplus/public/tomlplusplus.cppm
    bake/src/bake.util.cppm
    bake/src/bake.project.cppm
    bake/src/bake.moid.cppm
    bake/src/bake.graph.cppm
    bake/src/bake.compiler.cppm
    bake/src/bake.engine.cppm
    bake/src/bake.package.cppm
    bake/src/cli.cppm
)
```

Link LLVM/Clang/LLD/zlib/zstd directly to `bake`. Remove the CMake target named `core`, its `OUTPUT_NAME` property, project-library RPATH, Windows DLL definitions, and installation rule.

- [ ] **Step 4: Create the single-Moid self-build manifest**

Root `bake.toml`:

```toml
[workspace]
members = ["bake"]
```

`bake/bake.toml`:

```toml
[moid]
name = "bake"
version = "0.1.0"
type = "executable"
std = "c++23"

[dependencies]
tomlplusplus = { path = "../third_party/tomlplusplus" }
nlohmann_json = { path = "../third_party/nlohmann" }
```

`bake/build.cpp` declares all Bake sources, per-source `-fno-rtti` for the two LLVM files, LLVM include roots, version define, and static LLVM/Clang/LLD/zlib/zstd inputs. It calls `builder.executable("bake")`; it does not declare a separate library Moid.

- [ ] **Step 5: Install only the executable and source wrapper**

Use:

```cmake
install(TARGETS bake RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES lib/bake/bake.build.cppm DESTINATION lib/bake)
```

Keep runtime/header source installation in the existing packaging path; do not install a project library.

- [ ] **Step 6: Reconfigure from scratch without deleting user outputs**

Create a new build directory rather than cleaning existing outputs:

```bash
cmake -G Ninja -B build-single
cmake --build build-single
ctest --test-dir build-single --output-on-failure
```

Expected: one `bake` executable target, no `libbake.dylib`, and all tests pass.

- [ ] **Step 7: Verify dynamic dependencies**

Run:

```bash
otool -L build-single/bake
```

Expected for stage0: no project `libbake` dependency. Host libc++ is permitted only for stage0 bootstrap.

- [ ] **Step 8: Review checkpoint**

Search for `core/`, `libbake.dylib`, `DYLD_LIBRARY_PATH`, `BAKE_BUILDING_DLL`, and C ABI symbols. Remove obsolete references. Do not delete the old `build/` directory or any user output. Run `git diff --check`; do not commit without explicit approval.

---

### Task 10: Complete Stage1 Static Runtime Self-Build and Packaging Verification

**Files:**
- Modify: `bake/src/bake.compiler.cppm`
- Modify: `bake/src/bake.engine.cppm`
- Modify: `bake/src/cli.cppm`
- Modify: `bake/build.cpp`
- Modify: `scripts/update-runtime.sh`
- Modify: `CMakeLists.txt`
- Modify: `tests/test_runner.cpp`
- Modify: `AGENTS.md`

**Interfaces:**
- Consumes: ToolchainCache and typed graph artifacts from earlier tasks.
- Produces: a stage1 `out/bin/bake` that links vendored libc++/libc++abi runtime objects and has no project/runtime C++ dylib dependency.

- [ ] **Step 1: Add stage1 verification commands to the test workflow**

Use a temporary output/cache root and run:

```bash
./build-single/bake build
out/bin/bake --version
out/bin/bake build
```

The second build proves the produced Bake can execute and rebuild the workspace.

- [ ] **Step 2: Ensure executable runtime policy is planned before graph serialization**

For C++ executable/shared links, add cached libc++ and libc++abi objects as typed toolchain artifacts before writing `graph.json`. Add libunwind and compiler-rt only when target policy requires them. Never inject runtime inputs in CLI after `build_graph()`.

- [ ] **Step 3: Keep macOS SDK and deployment target coherent**

Use `xcrun --show-sdk-path` when SDK facilities are needed. If it fails, return no SDK; do not hard-code a fallback path. Read SDK metadata to select a deployment target that is not older than the selected libSystem stubs, or choose the configured user target when compatible. Vendored libc++ headers and runtime objects remain selected even in SDK mode.

- [ ] **Step 4: Verify Stage1 dependencies**

Run:

```bash
otool -L out/bin/bake
```

Expected: no `libbake.dylib`, `libc++.1.dylib`, or `libc++abi.dylib`. On macOS the expected platform dependency is libSystem plus any explicitly used Apple platform component.

- [ ] **Step 5: Assemble a copied distribution smoke directory**

Copy, without deleting existing paths:

```text
/tmp/bake-pkg/bin/bake
/tmp/bake-pkg/lib/bake/bake.build.cppm
/tmp/bake-pkg/lib/libc/
/tmp/bake-pkg/lib/libcxx/
/tmp/bake-pkg/lib/libcxxabi/
/tmp/bake-pkg/lib/libunwind/
/tmp/bake-pkg/lib/compiler-rt/
/tmp/bake-pkg/lib/clang/<version>/
```

Run pure C, pure C++, module, `build.cpp`, staticlib, and sharedlib fixtures using `/tmp/bake-pkg/bin/bake`. Assert no Homebrew compiler/linker process is spawned.

- [ ] **Step 6: Update runtime vendor script and project guidance**

Ensure `scripts/update-runtime.sh` refreshes only required libc++, libc++abi, libunwind, compiler-rt, and Darwin libc content in the flat `lib/` layout. Update `AGENTS.md` to describe Moids, four type names, Configure/Build JSON boundaries, final file layout, global cache, one Bake executable, and current verification commands.

- [ ] **Step 7: Run the complete verification matrix**

Run:

```bash
cmake --build build-single
ctest --test-dir build-single --output-on-failure
./build-single/bake build
out/bin/bake --version
out/bin/bake build
otool -L out/bin/bake
git diff --check
```

Expected: every test passes, self-build succeeds twice, Stage1 has no project/libc++/libc++abi dylib dependency, and the diff has no whitespace errors.

- [ ] **Step 8: Final cleanup audit**

Search the tracked tree for:

```text
PackageType
[package]
static-lib
shared-lib
libbake.dylib
core/
.pkgs
moid_decl_from_json_string
compile_flag_for
```

Every remaining hit must be either a historical design explanation or removed. Do not delete build/output directories the user asked to retain. Do not commit without explicit approval.
