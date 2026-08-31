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
local = { path = "../local" }                                  # path dep
remote = { url = "https://github.com/org/repo", tag = "v1.0" } # remote dep
flagged = { path = "../x", options = ["use_tls"] }             # activate features

[link]                    # platform-agnostic system linking
libraries = ["z"]         # -l flags

[target."*-apple-darwin"] # applies when the triple matches the glob
frameworks = ["Foundation"]

[target."*-linux-musl"]
libraries = ["pthread", "dl"]

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

| Key | Kind | Meaning |
|---|---|---|
| `path` | path dep | Relative directory containing a `bake.toml` (or a vendored source tree without one — see [Dependencies](../guide/dependencies.md)) |
| `url` | remote | Git URL |
| `tag` | remote | Tag to pin; resolved to a commit in `bake.lock` |
| `options` | any | List of the dependency's `[options]` to activate |

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
