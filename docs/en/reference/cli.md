# Command-Line Interface

```
bake <subcommand> [options]
bake <subcommand> --help
```

## Subcommands

| Command | Purpose |
|---|---|
| `bake init [name]` | Create a project scaffold |
| `bake build` | Build the project (or workspace) |
| `bake run [args…]` | Build, then run the executable; trailing positional args pass to the program |
| `bake test [name]` | Build and run registered tests |
| `bake add <url> [--tag <tag> \| --branch <branch> \| --rev <rev>] [name] [--target <glob>]` | Add a Git or archive dependency |
| `bake remove <name> [--target <glob>]` | Remove a dependency declaration |
| `bake update [dep]` | Re-resolve one dependency, or all dependencies when omitted |
| `bake clean` | Remove `out/` |
| `bake cc` / `bake c++` / `bake ar` | Invoke the embedded Clang/LLVM tools directly (`zig cc` style) |
| `bake audit` | Verify toolchain self-containment |

## Global options

| Flag | Meaning |
|---|---|
| `-V`, `--version` | Print version |
| `-h`, `--help` | Print help |

## Build options (`build`, `run`, `test`)

| Flag | Meaning |
|---|---|
| `--option <name>[=value]` | Override an `[options]` value from `bake.toml` |
| `-t <triple>`, `--target=<triple>` | Cross-compile target |
| `--release` | Build with the release profile |
| `--profile <name>` | Build with a specific profile |
| `-p <member>` | Build/test/run a specific workspace member |
| `-j <n>` | Parallel job count |
| `-v`, `--verbose` | Per-file compile progress |
| `--locked` | Fail if `bake.lock` is missing or stale |
| `--offline` | Never connect to the network |
| `--frozen` | `--locked` + `--offline` |

## `bake init`

```
bake init [name] [--type <executable|lib|dylib>] [--std <standard>]
```

- `--type` — moid type, default `executable`
- `--std` — `c11|c17|c23|c++17|c++20|c++23`, default `c++20`; C standards scaffold C sources
- Without `name`, scaffolds in the current directory
- Creates `bake.toml`, `src/`, `public/`, `tests/`, `.gitignore`; a `lib` scaffold adds `public/<name>/<name>.hpp`

## `bake add`

```
bake add <url> [--tag <tag> | --branch <branch> | --rev <rev>] [name] [--target <glob>]
```

- `--tag`, `--branch`, and `--rev` select a Git ref and are mutually exclusive.
- A URL ending in a supported archive suffix is added as a direct archive dependency automatically; archive dependencies cannot have a ref.
- `[name]` overrides the name inferred from the URL. An absolute local path is normalized to a `file://` URL.
- `--target <glob>` writes to `[target."<glob>".dependencies]`; an invalid glob is rejected immediately.
- In the selected scope, an identical existing declaration is skipped with a prompt to run `bake update`; a different declaration is replaced. The manifest edit preserves comments.

## `bake remove`

```
bake remove <name> [--target <glob>]
```

- With `--target`, removes the declaration from `[target."<glob>".dependencies]`.
- Without it, checks `[dependencies]` and then target dependency tables. If the name occurs in more than one scope, bake lists the matching scopes and requires `--target`.
- Removes lock entries no longer referenced by any dependency scope. Downloaded content remains in the cache.

## `bake update`

```
bake update [dep]
```

- `bake update <dep>` forces resolution for only that dependency's URL; unchanged entries for every other dependency are carried forward.
- `bake update` with no name re-resolves all dependencies. It is the only command that can move all locked refs.

## `bake test`

```
bake test [name] [--all] [-p <member>] [-j <n>]
```

- No arguments: run the default registered test (error if none)
- `name`: run that test
- `--all`: run every registered test
- Test binary output passes through; exit code non-zero if any selected test fails

## `bake run`

Builds, then runs the single executable of the selection. With several executables in the workspace, bake lists candidates and asks for `-p`. Trailing positional arguments are forwarded to the program; `-`-prefixed ones are consumed as bake options (no `--` separator).

## Exit codes

0 on success; 1 on any failure (build error, failed test, unsatisfiable `--locked`/`--offline`, missing manifest). Test failures propagate the failing binary's exit code semantics through `bake test`'s own non-zero exit.
