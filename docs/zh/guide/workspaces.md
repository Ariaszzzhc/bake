# 工作区

工作区是一个多包仓库：一个根 `bake.toml` 列出成员，每个成员都是带有自身 `bake.toml` 的完整包。

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

## 成员通过路径彼此依赖

在 `cli/bake.toml` 中：

```toml
[dependencies]
core = { path = "../core" }
codec = { path = "../codec" }
```

工作区内部的路径依赖会绕过包缓存，并作为同一依赖图的一部分构建——一次 `bake build` 会按依赖顺序并行编译所有内容。

## 构建

```bash
bake build          # the whole workspace
bake build -p cli   # one member (plus its dependencies)
bake run            # runs the single executable; -p picks if there are several
bake test -p core   # test one member
```

## 何时使用工作区

当多个包共享一个仓库并一同演进时，应使用工作区——其方式与 Cargo 工作区相同。具有远程依赖的独立包不需要工作区。