# 依赖

依赖在 `bake.toml` 的 `[dependencies]` 表中声明。依赖分为两种：路径依赖和远程依赖。

```toml
[dependencies]
# Path dependency: a local moid, resolved from disk
mylib = { path = "../mylib" }

# Remote dependency: fetched from a git URL, pinned to a tag
json = { url = "https://github.com/nlohmann/json", tag = "v3.11.3" }

# Feature activation: turn on the dependency's [options]
curl = { path = "../curl", options = ["use_tls"] }
```

## 路径依赖

路径依赖指向包含 `bake.toml` 的目录。每次构建都会从磁盘解析它们，并完全绕过包缓存——编辑依赖会使它作为构建的一部分重新构建。这是工作区成员彼此依赖的方式（参见[工作区](workspaces.md)）。

不含 `bake.toml` 的路径依赖会被视为供应商提供的头文件/源代码库：其 `public/` 目录会加入 include 路径，且其 `src/` 下任何可编译的源文件都会直接编译到你的 moid 中。这是使用普通检出库时无需额外配置的方式。

## 远程依赖

远程依赖是由 `tag` 固定的 git 仓库：

```toml
fmt = { url = "https://github.com/fmtlib/fmt", tag = "10.2.1" }
```

`bake add` 会为你写入此条目：

```bash
bake add https://github.com/fmtlib/fmt --tag 10.2.1
```

依赖名称默认是仓库名称（会移除 `.git` 后缀）；传入显式名称可覆盖它：`bake add <url> --tag <tag> my-fmt`。

## 锁文件：`bake.lock`

首次解析远程依赖的构建会写入 `bake.lock`，记录每个 tag 所解析到的确切提交：

```json
{
    "fmt": {
        "url": "https://github.com/fmtlib/fmt",
        "ref": "d5598e0f03e13c0b1b1b1e...",
        "ref_type": "tag"
    }
}
```

此后：

- **`bake build` 永不移动已锁定的 tag。** 构建可复现：会使用记录的提交，直到你显式更新。
- **`bake update [name]`** 会将 tag 重新解析为提交，并重写 `bake.lock`。指定名称参数时，仅更新该依赖；未指定时，更新所有远程依赖。路径依赖会被跳过（没有可解析的内容）。

## Locked、offline、frozen

| 标志 | 含义 |
|---|---|
| `--locked` | 若 `bake.lock` 缺失或已过期（`bake.toml` 中的 tag 未在锁文件中），则失败 |
| `--offline` | 绝不访问网络；若某项内容尚未在本地缓存中，则失败 |
| `--frozen` | 同时启用上述两项——适用于 CI 的配置 |

```bash
bake build --frozen     # exactly what the lock says, no network, no surprises
```

## 使用者如何使用你的库

若你的包是 `lib`，依赖者会获得你的公开头文件（其 include 路径中包含你的 `public/` 目录）、你的公开模块接口（已编译的 PCM）以及你的链接对象——并且会传递下去。除依赖本身外，它们无需在自身一侧声明任何内容；`static` 是默认值，私有依赖保持私有。

## 图中的选项

`[options]` 是仅允许 bool 的功能标志，会在整个依赖图中以 OR 合并（参见 [Profiles and Options](../reference/profiles.md)）。在 `curl` 依赖中激活 `use_tls` 会使该依赖（以及其下游的任何内容）以 `BAKE_CURL_USE_TLS=1` 编译。