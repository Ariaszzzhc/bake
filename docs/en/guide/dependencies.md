# Dependencies

Declare dependencies in `[dependencies]` and, when needed, in target-specific dependency tables. A dependency is a local path, a Git repository, or a directly downloaded archive.

```toml
[dependencies]
# Local path dependency
mylib = { path = "../mylib" }

# Git dependencies: choose at most one ref kind
fmt = { url = "https://github.com/fmtlib/fmt", tag = "11.0.2" }
catch2 = { url = "https://github.com/catchorg/Catch2", branch = "devel" }
fixed = { url = "https://github.com/org/repo", rev = "d5598e0f03e13c0b" }

# An archive URL is a direct archive dependency; it has no ref.
headers = { url = "https://example.com/headers-1.2.0.tar.gz" }
```

## Path dependencies and bare source packages

Path dependencies point at a local directory and are resolved from disk on every build. They bypass the package cache, so edits to the dependency participate in the next build immediately. This is how workspace members depend on each other; see [Workspaces](workspaces.md). Path dependencies are never written to `bake.lock`.

A path or fetched dependency without a `[package]` declaration is a *bare source package*. Consume its source directory from `build.cpp` with `b.dep_src_dir(alias)`:

```cpp
auto source_dir = b.dep_src_dir("headers");
```

This lets the build script decide how the checked-out source is used. bake emits a configure-time warning containing `never consumed` when a declared bare source dependency is never queried through `dep_src_dir`.

## Git dependencies

Git dependency entries use `url` and may select one ref:

```toml
[dependencies]
by_tag = { url = "https://github.com/fmtlib/fmt", tag = "11.0.2" }
by_branch = { url = "https://github.com/catchorg/Catch2", branch = "devel" }
by_revision = { url = "https://github.com/org/repo", rev = "d5598e0f03e13c0b" }
default_branch = { url = "https://github.com/org/repo" }
```

- `tag` selects a tag.
- `branch` selects a branch.
- `rev` selects an exact Git revision.
- With no ref, bake resolves the remote's default-branch `HEAD` at build time.

`tag`, `branch`, and `rev` are mutually exclusive: an entry may contain at most one of them. Each selected Git ref is resolved to a commit in the lockfile, so ordinary builds keep using that commit until `bake update` moves it.

Fetching: bake first downloads a Git dependency through the host's source
archive endpoint (GitHub/GitLab style `<repo>/archive/<commit>.tar.gz`) at
the locked commit; when the host has no such endpoint it falls back to a
full `git clone` checked out at that commit. Both transports produce the
same content-addressed source tree, so the lockfile integrity never depends
on which one was used.

### Archive dependencies

A URL ending in `.tar.gz`, `.tgz`, `.tar.bz2`, `.tbz2`, `.tar.xz`, `.txz`, or `.zip` is recognized as a direct archive dependency. Do not specify `tag`, `branch`, or `rev` for an archive URL; a ref on an archive dependency is an error.

Archive extraction uses external tools: tar archives use `tar`, and ZIP archives use `unzip`. The archive URL is locked by URL and its downloaded content is recorded with an integrity value.

## Target-specific dependencies

Use `[target."<triple-glob>".dependencies]` for dependencies that apply only to matching target triples. Its entries have exactly the same form as `[dependencies]`:

```toml
[dependencies]
fmt = { url = "https://github.com/fmtlib/fmt", tag = "11.0.2" }

[target."*-apple-darwin".dependencies]
metal_cpp = { url = "https://example.com/metal-cpp-1.0.0.zip" }

[target."*-linux-musl".dependencies]
musl_helpers = { url = "https://github.com/org/musl-helpers", branch = "main" }
```

For a build target, the effective dependency set is the union of `[dependencies]` and every target dependency table whose glob matches that target triple. The dependency graph is resolved from that effective set.

An alias may not have different definitions across scopes. If, for example, both `[dependencies]` and `[target."*-linux-musl".dependencies]` define `fmt` differently, bake fails and names both conflicting tables. The lockfile nevertheless covers the union of dependencies from all scopes, not only the host target.

## Resolution and `bake.lock`

`bake.lock` records resolved non-path dependencies. Git entries use keys of the form `git:<url>@<commit>`; archive entries use `archive:<url>`. Every entry records `url`, `ref`, `ref_type`, `commit`, and `integrity`; `name` is present when the resolved native package declares `[package].name`.

```json
{
  "git:https://github.com/fmtlib/fmt@d5598e0f03e13c0b1b1b1e...": {
    "url": "https://github.com/fmtlib/fmt",
    "ref": "11.0.2",
    "ref_type": "tag",
    "commit": "d5598e0f03e13c0b1b1b1e...",
    "integrity": "sha256-...",
    "name": "fmt"
  },
  "archive:https://example.com/headers-1.2.0.tar.gz": {
    "url": "https://example.com/headers-1.2.0.tar.gz",
    "ref": "",
    "ref_type": "archive",
    "commit": "",
    "integrity": "sha256-..."
  }
}
```

### Incremental resolution

When a build or update refreshes a lockfile, an existing entry whose URL and ref are unchanged is carried over unchanged. bake does not run `git ls-remote` or re-resolve it. Only newly added or changed dependencies, and dependencies explicitly selected by `bake update <dep>`, need network resolution. If a carried entry has no local cache (e.g. a fresh clone with a committed lockfile), `bake build` re-downloads it by its locked commit/URL and verifies the tree hash — the ref does not move.

`bake update` is the only command that can move locked refs. Without a dependency name, it re-resolves every dependency and can move every locked ref; `bake build` does not move locked refs.

### Locked, offline, frozen

| Flag | Meaning |
|---|---|
| `--locked` | Fail if `bake.lock` is missing or stale. |
| `--offline` | Never touch the network; fail if required content is not already available locally. |
| `--frozen` | Both of the above — the CI configuration. |

```bash
bake build --frozen     # exactly what the lock says, no network, no surprises
```

## Managing dependencies

Add a dependency with:

```bash
bake add <url> [--tag <tag> | --branch <branch> | --rev <rev>] [name] [--target <glob>]
```

The optional ref flags select a Git ref; archive URLs are recognized automatically and must not receive a ref. An absolute local path is normalized to a `file://` URL. The name defaults from the URL, or supply `[name]` explicitly. `--target <glob>` inserts the declaration in `[target."<glob>".dependencies]`; an invalid glob fails immediately. bake performs a comment-preserving textual insertion.

If the selected scope already has the name, an identical declaration is skipped with a prompt to run `bake update`; a different declaration replaces it.

Remove a dependency with:

```bash
bake remove <name> [--target <glob>]
```

Without `--target`, bake checks `[dependencies]` first and then target dependency tables. A name found in more than one scope is an error that lists every matching scope and requires `--target`. Removal prunes lock entries no longer referenced by any scope, but never removes downloaded content from the cache.

To refresh a dependency's resolved URL, run `bake update <dep>`; it forces resolution only for that dependency and carries every other unchanged entry forward. Run `bake update` with no name to re-resolve all dependencies.

## How consumers use your library

If your package is a `lib`, dependents get your public headers (your `public/` directory on their include path), your public module interfaces (compiled PCMs), and your linked objects — transitively. There is nothing to declare on their side beyond the dependency itself; `static` is the default and private dependencies stay private.

## Options across the graph

`[features]` are named capability bundles, unified by union across the whole dependency graph (see [Profiles and Features](../reference/profiles.md)). Activating `tls-openssl` on your `curl` dependency resolves that feature's dependencies (the openssl port), injects its `defines` (note: `defines` apply only to the dependency's own translation units; reaching yours requires `public_defines`), and compiles the dependency with `BAKE_CURL_TLS_OPENSSL=1`; conflicts follow explicit-beats-default — an explicit activation demotes a conflicting default (the default `tls-mbedtls` yields to your explicit `tls-openssl`); only two explicit activations clashing fail at configure time. To trim a dependency's default features, declare `default-features = false` on it — that edge then contributes only the features explicitly named in `features`, and the union from other edges is unaffected (bake warns on mixed edges). For example, freetype without PNG: `freetype = { url = "...", tag = "v2.14.3", default-features = false }`.
