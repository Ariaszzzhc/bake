# Profiles and Features

## Profiles

A profile is a named compiler configuration. Select with:

```bash
bake build              # dev profile (default)
bake build --release    # release profile
bake build --profile bench   # any [profile.bench] you define
```

### Built-in profiles

| Field | `dev` | `release` |
|---|---|---|
| `opt` | 0 | 3 |
| `debug` | true | false |
| `lto` | — | true (thin) |
| `warnings` | `"all"` | `"all"` |

### Defining and overriding

`[profile.<name>]` overrides the built-in of the same name (or defines a new profile — custom names start from an empty base, so set every field you care about):

```toml
[profile.release]
opt = 2
lto = false
strip = true

[profile.bench]
opt = 3
debug = false
warnings = "extra"
```

### Fields

| Key | Type | Meaning |
|---|---|---|
| `opt` | int 0–3 | Optimization level (`-O0` … `-O3`) |
| `debug` | bool | Debug info (`-g`) |
| `lto` | bool | Link-time optimization (thin LTO) |
| `strip` | bool | Strip the final binary |
| `sanitize` | array | Sanitizers, e.g. `["address"]` |
| `warnings` | string | `"none"` \| `"all"` \| `"extra"` \| `"error"` |

Only fields you set are overridden; the rest of the base profile stands. Profile selection does not invalidate other profiles' output — each (`profile`, `target`) combination is fingerprinted independently.

## Features

Features are the capability system — declared in `[features]`, unified by union across the graph (see the [Manifest reference](manifest.md#features)):

```toml
# mylib/bake.toml
[features]
default = ["core"]
use-tls = { dependencies = { mbedtls = { url = "...", tag = "v3.6.7" } },
            defines = ["USE_TLS"] }
```

Activation paths:

1. **Dependency edge** — `flagged = { path = "../x", features = ["use-tls"] }` in a dependent's `[dependencies]`.
2. **CLI** — `bake build --feature use-tls` (root moid only).
3. **Default set** — the declaring `bake.toml`'s `default` list.

The effective set is the union of all three. Every declared feature yields a macro (`1` active / `0` inactive):

```cpp
#if BAKE_MYLIB_USE_TLS
    // compiled only when the feature is active
#endif
```

In `build.cpp`, read features programmatically:

```cpp
if (b.feature("use-tls"))
    b.sources("src/tls/*.cpp");
```

This is the intended way to make input sets feature-dependent — the feature macro gates code, `feature()` gates which files compile at all, and the feature's `dependencies` gate which packages enter the graph.
