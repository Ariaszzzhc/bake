# Profiles and Options

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

## Options

Options are the feature-flag system — bool-only, declared in `[options]`, propagated across the graph:

```toml
# mylib/bake.toml
[options]
use_tls = false
```

Activation paths:

1. **Dependency edge** — `flagged = { path = "../x", options = ["use_tls"] }` in a dependent's `[dependencies]`.
2. **CLI** — `bake build --option use_tls` (or `--option use_tls=false`).
3. **Default** — the value in the declaring `bake.toml`.

The resolved value is the OR of all activations. In code:

```cpp
#ifdef BAKE_MYLIB_USE_TLS
    // compiled only when the feature is on
#endif
```

In `build.cpp`, read options programmatically:

```cpp
if (b.option_bool("use_tls"))
    b.sources("src/tls/*.cpp");
```

This is the intended way to make input sets feature-dependent — the option macro gates code, `option_bool` gates which files compile at all.
