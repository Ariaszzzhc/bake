# Profile 和选项

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

## 选项

选项构成特性开关系统——仅限 bool，声明在 `[options]` 中，并在图中传播：

```toml
# mylib/bake.toml
[options]
use_tls = false
```

启用路径：

1. **依赖边** — 在依赖方的 `[dependencies]` 中使用 `flagged = { path = "../x", options = ["use_tls"] }`。
2. **CLI** — `bake build --option use_tls`（或 `--option use_tls=false`）。
3. **默认值** — 声明该选项的 `bake.toml` 中的值。

解析后的值是所有启用来源的 OR。代码中：

```cpp
#ifdef BAKE_MYLIB_USE_TLS
    // compiled only when the feature is on
#endif
```

在 `build.cpp` 中，以编程方式读取选项：

```cpp
if (b.option_bool("use_tls"))
    b.sources("src/tls/*.cpp");
```

这是使输入集合依赖于特性的预期方式——选项宏控制代码是否启用，`option_bool` 控制哪些文件会被编译。
