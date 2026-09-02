# Profile 与特性

## Profile

profile 是具名的编译器配置。通过如下方式选择：

```bash
bake build              # dev profile (default)
bake build --release    # release profile
bake build --profile bench   # any [profile.bench] you define
```

### 内置 profile

| 字段 | `dev` | `release` |
|---|---|---|
| `opt` | 0 | 3 |
| `debug` | true | false |
| `lto` | — | true (thin) |
| `warnings` | `"all"` | `"all"` |

### 定义与覆盖

`[profile.<name>]` 会覆盖同名的内置 profile（也可以定义新的 profile——自定义名称从空基础开始，因此请设置所有需要的字段）：

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

### 字段

| 键 | 类型 | 含义 |
|---|---|---|
| `opt` | int 0–3 | 优化级别（`-O0` … `-O3`） |
| `debug` | bool | 调试信息（`-g`） |
| `lto` | bool | 链接时优化（thin LTO） |
| `strip` | bool | 剥离最终 binary |
| `sanitize` | array | Sanitizer，例如 `["address"]` |
| `warnings` | string | `"none"` \| `"all"` \| `"extra"` \| `"error"` |

只有已设置的字段会被覆盖；基础 profile 的其余字段保持不变。Profile 的选择不会使其他 profile 的输出失效——每个（`profile`、`target`）组合均独立计算指纹。

## 特性

特性是能力开关系统——声明在 `[features]` 中，在图上并集合一（详见 [Manifest 参考](manifest.md#features)）：

```toml
# mylib/bake.toml
[features]
default = ["core"]
use-tls = { dependencies = { mbedtls = { url = "...", tag = "v3.6.7" } },
            defines = ["USE_TLS"] }
```

激活路径：

1. **依赖边** — 在依赖方的 `[dependencies]` 中使用 `flagged = { path = "../x", features = ["use-tls"] }`。
2. **CLI** — `bake build --feature use-tls`（仅 root moid）。
3. **默认集** — 声明方 `bake.toml` 的 `default` 列表。

有效集 = 三者之并。每个声明特性产生宏（激活 `1` / 未激活 `0`）：

```cpp
#if BAKE_MYLIB_USE_TLS
    // 仅在特性激活时编译
#endif
```

在 `build.cpp` 中，以编程方式读取：

```cpp
if (b.feature("use-tls"))
    b.sources("src/tls/*.cpp");
```

这是使输入集合依赖于特性的预期方式——特性宏控制代码是否启用，`feature()` 控制哪些文件会被编译；特性的 `dependencies` 控制哪些包进入图。
