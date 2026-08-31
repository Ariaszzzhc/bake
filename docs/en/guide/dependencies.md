# Dependencies

Dependencies are declared in the `[dependencies]` table of `bake.toml`. There are two kinds: path dependencies and remote dependencies.

```toml
[dependencies]
# Path dependency: a local moid, resolved from disk
mylib = { path = "../mylib" }

# Remote dependency: fetched from a git URL, pinned to a tag
json = { url = "https://github.com/nlohmann/json", tag = "v3.11.3" }

# Feature activation: turn on the dependency's [options]
curl = { path = "../curl", options = ["use_tls"] }
```

## Path dependencies

Path dependencies point at a directory containing a `bake.toml`. They are resolved from disk on every build and bypass the package cache entirely — editing the dependency rebuilds it as part of your build. This is how workspace members depend on each other (see [Workspaces](workspaces.md)).

A path dependency that has **no** `bake.toml` is treated as a vendored header/source library: its `public/` directory is added to your include path, and any compilable sources under its `src/` are compiled directly into your moid. This is the zero-ceremony way to consume a plain checked-out library.

## Remote dependencies

Remote dependencies are git repositories pinned by `tag`:

```toml
fmt = { url = "https://github.com/fmtlib/fmt", tag = "10.2.1" }
```

`bake add` writes this entry for you:

```bash
bake add https://github.com/fmtlib/fmt --tag 10.2.1
```

The dependency name defaults to the repository name (a `.git` suffix is stripped); pass an explicit name to override: `bake add <url> --tag <tag> my-fmt`.

## The lockfile: `bake.lock`

The first build that resolves a remote dependency writes `bake.lock`, recording the exact commit each tag resolved to:

```json
{
    "fmt": {
        "url": "https://github.com/fmtlib/fmt",
        "ref": "d5598e0f03e13c0b1b1b1e...",
        "ref_type": "tag"
    }
}
```

From then on:

- **`bake build` never moves a locked tag.** Builds are reproducible: the recorded commit is used until you explicitly update.
- **`bake update [name]`** re-resolves tags to commits and rewrites `bake.lock`. With a name argument, only that dependency; without, all remote dependencies. Path dependencies are skipped (nothing to resolve).

## Locked, offline, frozen

| Flag | Meaning |
|---|---|
| `--locked` | Fail if `bake.lock` is missing or stale (a tag in `bake.toml` is not in the lock) |
| `--offline` | Never touch the network; fail if something is not already in the local cache |
| `--frozen` | Both of the above — the CI configuration |

```bash
bake build --frozen     # exactly what the lock says, no network, no surprises
```

## How consumers use your library

If your package is a `lib`, dependents get your public headers (your `public/` directory on their include path), your public module interfaces (compiled PCMs), and your linked objects — transitively. There is nothing to declare on their side beyond the dependency itself; `static` is the default and private dependencies stay private.

## Options across the graph

`[options]` are bool-only feature flags, OR-merged across the whole dependency graph (see [Profiles and Options](../reference/profiles.md)). Activating `use_tls` on your `curl` dependency compiles that dependency (and anything downstream) with `BAKE_CURL_USE_TLS=1`.
