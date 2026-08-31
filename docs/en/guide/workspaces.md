# Workspaces

A workspace is a multi-package repository: one root `bake.toml` listing members, each member a full package with its own `bake.toml`.

```toml
# ./bake.toml (workspace root)
[workspace]
members = ["cli", "core", "codec"]
```

```
.
├── bake.toml          # workspace root: members only
├── cli/
│   ├── bake.toml
│   └── src/
├── core/
│   ├── bake.toml
│   └── src/
└── codec/
    ├── bake.toml
    └── src/
```

## Members depend on each other via path

Inside `cli/bake.toml`:

```toml
[dependencies]
core = { path = "../core" }
codec = { path = "../codec" }
```

Workspace-internal path dependencies bypass the package cache and build as part of the same graph — one `bake build` compiles everything, in dependency order, in parallel.

## Building

```bash
bake build          # the whole workspace
bake build -p cli   # one member (plus its dependencies)
bake run            # runs the single executable; -p picks if there are several
bake test -p core   # test one member
```

## When to use a workspace

Use a workspace whenever several packages share a repository and evolve together — the way Cargo workspaces do. Standalone packages with remote dependencies do not need one.
