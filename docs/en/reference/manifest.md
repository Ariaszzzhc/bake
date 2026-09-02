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

[options]                 # bool-only feature flags
use_tls = false
use_json = true

[dependencies]
local = { path = "../local" }                                      # path dep
by_tag = { url = "https://github.com/org/repo", tag = "v1.0" }   # Git tag
by_branch = { url = "https://github.com/org/repo", branch = "main" } # Git branch
by_rev = { url = "https://github.com/org/repo", rev = "d5598e..." }  # exact Git revision
head = { url = "https://github.com/org/head" }                     # default-branch HEAD
archive = { url = "https://example.com/lib-1.0.tar.xz" }           # direct archive
flagged = { path = "../x", options = ["use_tls"] }                # activate features

[link]                    # platform-agnostic system linking
libraries = ["z"]         # -l flags

[target."*-apple-darwin"] # applies when the triple matches the glob
frameworks = ["Foundation"]

[target."*-linux-musl"]
libraries = ["pthread", "dl"]

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

## `[options]`

Bool-only feature flags. Each becomes a preprocessor macro `BAKE_<MOID>_<OPTION>` set to `1` or `0` (macro names are upper-cased). Options are OR-merged across the dependency graph: if any package activates a dependency's option, it is on everywhere. CLI override:

```bash
bake build --option use_tls        # =true
bake build --option use_tls=false
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
| `options` | any dependency | List of the dependency's `[options]` to activate. |

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

Matching sections merge with `[link]` and each other.

## `[profile.<name>]`

See [Profiles and Options](profiles.md) for the full field list and built-in defaults.

## `[sources]`

Extension lists used by default discovery to classify files:

- `module_ext` — C++ module interfaces (`.cppm`, `.ixx`)
- `source_ext` — ordinary translation units (`.cpp`, `.cc`, `.cxx`, `.c`)
- `header_ext` — headers, for tooling/IDE benefit; never compiled

A package is a *moid* — one `bake.toml` describes one compilation unit set. Multiple packages live in a [workspace](../guide/workspaces.md).
