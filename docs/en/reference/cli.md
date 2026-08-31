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
| `bake add <url> --tag <tag> [name]` | Add a remote dependency to `bake.toml` |
| `bake update [dep]` | Re-resolve tags → commits, update `bake.lock` |
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
