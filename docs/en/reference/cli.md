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
| `bake compdb` | Write `compile_commands.json` from the resolved build graph (no build) |
| `bake lsp` | Language server over stdio (self-hosted binary only) |

## Global options

| Flag | Meaning |
|---|---|
| `-V`, `--version` | Print version |
| `-h`, `--help` | Print help |

## Build options (`build`, `run`, `test`)

| Flag | Meaning |
|---|---|
| `--feature <name>` | Activate a root `[features]` feature |
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
- `name` may be a path: the project is created there and named after the last component (`bake init tools/hello` → `tools/hello/` with `name = "hello"`). Project names must be portable file names — `[A-Za-z0-9][A-Za-z0-9._+@-]*`, no trailing dot, no reserved device names like `CON`.
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

## `bake compdb`

```
bake compdb [--release | --profile <name>] [-t <triple>] [--offline] [output]
```

- Resolves the build graph exactly like `bake build` (same selection, profile
  and target handling) but executes nothing, then writes a
  [JSON compilation database](https://clang.llvm.org/docs/JSONCompilationDatabase.html)
  — `compile_commands.json` at the project root, or `[output]` when given.
- `bake build` refreshes the same file after every build; `bake compdb` is
  the no-build path (and what the language server's compile queries use).
- `--offline` (default behavior for the LSP) refuses network resolution;
  unresolved dependencies surface as errors pointing at `bake update`.

## `bake lsp`

```
bake lsp
```

- Speaks the Language Server Protocol over stdio (JSON-RPC, Content-Length
  framing). Editors launch it like any LSP server; the workspace root comes
  from the `initialize` handshake.
- Current surface: lifecycle (`initialize`/`shutdown`/`exit`), full document
  sync (`didOpen`/`didChange`/`didClose`), diagnostics, completion
  (`textDocument/completion`), definition and references
  (`textDocument/definition`, `textDocument/references`).
- Compiles run in-process against the command `bake build` would use, with
  the editor buffer remapped over the file — diagnostics always reflect
  build truth, never a guessed compilation database.
- Preamble regions compile once into PCHs, content-keyed in the global
  cache; keystroke compiles parse the edited body only (module-importing
  preambles fall back to whole-file compiles).
- A low-priority background thread indexes project sources into per-TU
  symbol shards; definition/references answer from the open file first
  and the shard set second, so cross-file jumps work on a cold index too.
- Only present in the self-hosted binary (it rides on the kotatsu async
  runtime); the stage0 bootstrap prints an explanatory error.

## Exit codes
0 on success; 1 on any failure (build error, failed test, unsatisfiable `--locked`/`--offline`, missing manifest). Test failures propagate the failing binary's exit code semantics through `bake test`'s own non-zero exit.
