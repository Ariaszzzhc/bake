# Manifest Format (`bake.toml`)

Every key bake reads, in one page. Unknown keys are ignored.

```toml
# Workspace root only: list member directories
[workspace]
members = ["cli", "core"]

[package]
name = "myapp"            # required
version = "0.1.0"
type = "executable"       # executable (default) | lib | dylib

[language]                # defaults: c++17 / c17
cxx = "c++23"             # c++17 | c++20 | c++23
c = "c17"                 # c11 | c17 | c23
# C++20 or later is required for `import std;` and named modules.

[features]                # capability bundles: deps / defines / conflicts / platforms
default = ["tls", "zlib"]
tls = { dependencies = { mbedtls = { url = "...", tag = "v3.6.7" } }, defines = ["USE_MBEDTLS"] }
zlib = {}

[dependencies]
local = { path = "../local" }                                      # path dep
by_tag = { url = "https://github.com/org/repo", tag = "v1.0" }   # Git tag
by_branch = { url = "https://github.com/org/repo", branch = "main" } # Git branch
by_rev = { url = "https://github.com/org/repo", rev = "d5598e..." }  # exact Git revision
head = { url = "https://github.com/org/head" }                     # default-branch HEAD
archive = { url = "https://example.com/lib-1.0.tar.xz" }           # direct archive
flagged = { path = "../x", features = ["use-tls"] }              # activate features

[link]                    # platform-agnostic system linking
libraries = ["z"]         # -l flags

[target."*-apple-darwin"] # applies when the triple matches the glob
frameworks = ["Foundation"]

[target."*-linux-musl"]
libraries = ["pthread", "dl"]
defines = ["Z_HAVE_UNISTD_H"]   # upstream feature macros (target tables only)

[target."*-apple-darwin".dependencies]
metal_cpp = { url = "https://example.com/metal-cpp-1.0.0.zip" }

[profile.release]         # override the built-in release profile
opt = 3
lto = true
strip = true

[sources]                 # file extension configuration (defaults shown)
module_ext = [".cppm", ".ixx"]
source_ext = [".cpp", ".cc", ".cxx", ".c"]
header_ext = [".h", ".hpp", ".hxx", ".hh"]
```

## `[package]`

| Key | Type | Default | Meaning |
|---|---|---|---|
| `name` | string | required | Moid name; drives output file names and option macros |
| `version` | string | `"0.1.0"` | Semver-ish; exposed as `BAKE_<NAME>_VERSION` macros |
| `type` | string | `"executable"` | `executable` → `out/<t>/bin/`; `lib` → static archive + public interfaces; `dylib` → shared library |

## `[language]`

`cxx` and `c` set the language standard for C++ and C sources respectively. Defaults: `c++17` / `c17`. (`bake init --std` writes this table for you.)

## `[features]`

Named capability bundles. A feature can carry four things:

```toml
[features]
default = ["tls-mbedtls", "zlib"]     # reserved: the activation set when nothing else is requested

tls-mbedtls = {
  platforms = ["*-apple-darwin", "*-linux-*"],   # applies only to these targets; omitted = all
  dependencies = { mbedtls = { url = "...", tag = "v3.6.7" } },  # same entry grammar as [dependencies]; resolved only when active
  defines = ["USE_MBEDTLS"],                     # macros injected into this package's compiles ("NAME" or "NAME=VALUE")
  conflicts = ["tls-openssl"],                   # mutually exclusive feature names
}
zlib = {}                             # macro-only feature: just BAKE_<MOID>_ZLIB
```

Activation semantics:

- **Union unification**: a package's effective set is its own `default` ∪ all incoming edge activations ∪ CLI. `build.cpp` queries with `b.feature("zlib")`
- Every declared feature yields the macro `BAKE_<MOID>_<FEATURE>` — `1` when active, `0` when not
- **conflicts** follow explicit-beats-default: a default feature conflicting with an explicitly activated one (dependency edge/CLI) is demoted with a printed note — its dependencies and defines disappear — so consumers can switch a package's default backend; two explicit or two default features clashing fail at configure time, and the error names both activation origins
- **platforms** is the applicability whitelist: a default-set feature that does not match the build target contributes nothing silently; an explicitly activated one (dependency edge or `--feature`) that does not match is an error
- A feature's dependencies enter the graph, the lockfile, and the download queue only when active

CLI activation (root moid only):

```bash
bake build --feature tls-openssl
```

Every moid also gets version macros:

```c
BAKE_MYAPP_NAME = "myapp"
BAKE_MYAPP_VERSION = "0.1.0"
BAKE_MYAPP_VERSION_MAJOR = 0
BAKE_MYAPP_VERSION_MINOR = 1
BAKE_MYAPP_VERSION_PATCH = 0
```

## `[dependencies]`

Each alias in `[dependencies]` declares one dependency. The same entry syntax is used by `[target."<glob>".dependencies]`.

| Key | Kind | Meaning |
|---|---|---|
| `path` | path dependency | Local directory. It is resolved from disk on each build and is never entered in `bake.lock`. |
| `url` | Git or archive dependency | A Git URL, unless it ends in `.tar.gz`, `.tgz`, `.tar.bz2`, `.tbz2`, `.tar.xz`, `.txz`, or `.zip`; those suffixes declare a direct archive dependency. |
| `tag` | Git ref | Select a tag. Mutually exclusive with `branch` and `rev`. |
| `branch` | Git ref | Select a branch. Mutually exclusive with `tag` and `rev`. |
| `rev` | Git ref | Select an exact Git revision. Mutually exclusive with `tag` and `branch`. |
| `features` | any dependency | List of the dependency's `[features]` to activate. |

A Git dependency with no `tag`, `branch`, or `rev` resolves its default-branch `HEAD` at build time. Archive dependencies must not specify a ref. Tar archives are extracted with `tar`; ZIP archives use `unzip`.

## `[target."<glob>".dependencies]`

Target dependency tables use the same alias entries as `[dependencies]`. For a build target, bake uses the union of global dependencies and every target dependency table whose glob matches the current target triple. The lockfile covers the union across all scopes, while graph resolution uses only the current target's effective set.

Do not define an alias differently in two scopes: bake reports an error naming both conflicting tables. See [Dependencies](../guide/dependencies.md) for commands and resolution behavior.

## `bake.lock`

The lockfile records every non-path dependency. Git keys are `git:<url>@<commit>` and archive keys are `archive:<url>`. Each entry contains `url`, `ref`, `ref_type`, `commit`, and `integrity`; an optional `name` records a resolved native package's `[package].name`. URL/ref-identical entries are carried forward unchanged during incremental resolution, without another remote lookup or download.

## `[link]` and `[target."…"]`

`[link]` applies on every platform; `[target."<glob>"]` sections apply only when the active target triple matches the glob (`*` matches one complete segment). Both accept:

- `libraries` — system libraries to link (`-l`)
- `frameworks` — macOS frameworks

`[target."<glob>"]` sections additionally accept:

- `defines` — preprocessor macros passed when compiling this package (`"NAME"` or `"NAME=VALUE"`)
- `flags` — extra compiler flags appended to compile commands
- `include_dirs` — extra include paths

Matching sections merge with `[link]` and each other. Target tables are where platform differences of an upstream library — feature macros its `configure` would otherwise detect — get declared. Porting zlib to non-Windows targets is just `defines = ["Z_HAVE_UNISTD_H"]`.

## `[profile.<name>]`

See [Profiles and Options](profiles.md) for the full field list and built-in defaults.

## `[sources]`

Extension lists used by default discovery to classify files:

- `module_ext` — C++ module interfaces (`.cppm`, `.ixx`)
- `source_ext` — ordinary translation units (`.cpp`, `.cc`, `.cxx`, `.c`)
- `header_ext` — headers, for tooling/IDE benefit; never compiled

A package is a *moid* — one `bake.toml` describes one compilation unit set. Multiple packages live in a [workspace](../guide/workspaces.md).
