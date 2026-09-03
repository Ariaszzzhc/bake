# 依赖

依赖在 `[dependencies]` 中声明；需要按目标区分时，则在目标专属依赖表中声明。依赖可以是本地路径、Git 仓库或直接下载的归档文件。

```toml
[dependencies]
# 本地路径依赖
mylib = { path = "../mylib" }

# Git 依赖：至多选择一种 ref 类型
fmt = { url = "https://github.com/fmtlib/fmt", tag = "11.0.2" }
catch2 = { url = "https://github.com/catchorg/Catch2", branch = "devel" }
fixed = { url = "https://github.com/org/repo", rev = "d5598e0f03e13c0b" }

# 归档 URL 是直接归档依赖；不带 ref。
headers = { url = "https://example.com/headers-1.2.0.tar.gz" }
```

## 路径依赖与裸源码包

路径依赖指向本地目录，并在每次构建时从磁盘解析。它们绕过包缓存，因此对依赖的编辑会立即参与下一次构建。这是工作区成员彼此依赖的方式；参见[工作区](workspaces.md)。路径依赖永不写入 `bake.lock`。

不带 `[package]` 声明的路径或已获取依赖是*裸源码包*。在 `build.cpp` 中通过 `b.dep_src_dir(alias)` 使用其源码目录：

```cpp
auto source_dir = b.dep_src_dir("headers");
```

这样由构建脚本决定如何使用检出的源码。如果一个已声明的裸源码依赖从未通过 `dep_src_dir` 查询，bake 会在 configure 阶段发出包含 `never consumed` 的警告。

## Git 依赖

Git 依赖条目使用 `url`，并且可以选择一个 ref：

```toml
[dependencies]
by_tag = { url = "https://github.com/fmtlib/fmt", tag = "11.0.2" }
by_branch = { url = "https://github.com/catchorg/Catch2", branch = "devel" }
by_revision = { url = "https://github.com/org/repo", rev = "d5598e0f03e13c0b" }
default_branch = { url = "https://github.com/org/repo" }
```

- `tag` 选择标签。
- `branch` 选择分支。
- `rev` 选择确切的 Git revision。
- 不带 ref 时，bake 会在构建时解析远端默认分支的 `HEAD`。

`tag`、`branch` 和 `rev` 互斥：一个条目至多包含其中一个。每个选定的 Git ref 都会在锁文件中解析为 commit，因此普通构建会持续使用该 commit，直到 `bake update` 移动它。

取回方式：bake 优先通过托管方的源码归档端点（GitHub/GitLab 风格的
`<仓库>/archive/<commit>.tar.gz`）按锁定的 commit 下载 Git 依赖；端点不存在或不可用时回退到完整
`git clone` 并检出该 commit。两种取回方式产生同一棵内容寻址源码树，锁文件的完整性值不依赖取回方式。

### 归档依赖

以 `.tar.gz`、`.tgz`、`.tar.bz2`、`.tbz2`、`.tar.xz`、`.txz`、`.tar.zst`、`.tzst` 或 `.zip` 结尾的 URL 会被识别为直接归档依赖。归档 URL 不要指定 `tag`、`branch` 或 `rev`；归档依赖带 ref 会报错。

自举后的 bake 内嵌 libcurl 与 libarchive，下载与解压都在进程内完成；stage-0 引导二进制则调用系统 `curl`、`tar`、`unzip`。Git 传输（ref 解析、clone 兜底）仍使用 `git` 二进制。归档 URL 按 URL 锁定，已下载内容会记录完整性值。

## 目标专属依赖

使用 `[target."<triple-glob>".dependencies]` 为匹配的目标三元组声明专属依赖。其条目形式与 `[dependencies]` 完全相同：

```toml
[dependencies]
fmt = { url = "https://github.com/fmtlib/fmt", tag = "11.0.2" }

[target."*-apple-darwin".dependencies]
metal_cpp = { url = "https://example.com/metal-cpp-1.0.0.zip" }

[target."*-linux-musl".dependencies]
musl_helpers = { url = "https://github.com/org/musl-helpers", branch = "main" }
```

对某个构建目标而言，有效依赖集是 `[dependencies]` 与 glob 匹配该目标三元组的每个目标依赖表的并集。依赖图从这个有效集解析。

一个 alias 不能在不同作用域中具有不同定义。例如 `[dependencies]` 与 `[target."*-linux-musl".dependencies]` 都以不同方式定义 `fmt` 时，bake 会失败并点名两个冲突的表。不过锁文件覆盖所有作用域中依赖的并集，而不只覆盖主机目标。

## 解析与 `bake.lock`

`bake.lock` 记录所有非路径依赖的解析结果。Git 条目的键形式为 `git:<url>@<commit>`；归档条目的键形式为 `archive:<url>`。每个条目都记录 `url`、`ref`、`ref_type`、`commit` 和 `integrity`；当解析出的原生包声明 `[package].name` 时，还会包含 `name`。

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

### 增量解析

当构建或更新需要刷新锁文件时，URL 和 ref 未变的已有条目会原样搬运。bake 不会再次运行 `git ls-remote`，也不会重新解析它。只有新增或变更的依赖，以及由 `bake update <dep>` 显式选中的依赖，才需要网络解析。若搬运条目在本机没有缓存（例如新克隆的仓库带着已提交的锁文件），`bake build` 会按锁定的 commit/URL 重新下载并校验树哈希——ref 不发生移动。

`bake update` 是唯一会移动已锁定 ref 的命令。不带依赖名称时，它会重新解析所有依赖，并可能移动所有已锁定 ref；`bake build` 不会移动已锁定的 ref。

### Locked、offline、frozen

| 标志 | 含义 |
|---|---|
| `--locked` | 若 `bake.lock` 缺失或已过期，则失败。 |
| `--offline` | 绝不访问网络；若所需内容尚未在本地可用，则失败。 |
| `--frozen` | 同时启用上述两项——适用于 CI 的配置。 |

```bash
bake build --frozen     # 完全遵从锁文件，不访问网络，不出意外
```

## 管理依赖

添加依赖：

```bash
bake add <url> [--tag <tag> | --branch <branch> | --rev <rev>] [name] [--target <glob>]
```

可选 ref 标志选择 Git ref；归档 URL 会自动识别，且不得带 ref。绝对本地路径会规范化为 `file://` URL。名称默认从 URL 推导，也可显式提供 `[name]`。`--target <glob>` 会将声明插入 `[target."<glob>".dependencies]`；非法 glob 会立即失败。bake 通过保留注释的文本插入完成修改。

若所选作用域已有该名称，完全相同的声明会被跳过，并提示运行 `bake update`；不同的声明会替换原声明。

删除依赖：

```bash
bake remove <name> [--target <glob>]
```

未指定 `--target` 时，bake 会先检查 `[dependencies]`，再检查目标依赖表。若一个名称在多个作用域中出现，会报错并列出所有匹配作用域，要求提供 `--target`。删除会剪除不再被任何作用域引用的锁条目，但绝不从缓存删除已下载内容。

要刷新一个依赖的已解析 URL，运行 `bake update <dep>`；它只强制解析该依赖，并原样搬运其他未变条目。运行不带名称的 `bake update` 则重新解析全部依赖。

## 使用者如何使用你的库

若你的包是 `lib`，依赖者会获得你的公开头文件（其 include 路径中包含你的 `public/` 目录）、你的公开模块接口（已编译的 PCM）以及你的链接对象——并且会传递下去。除依赖本身外，它们无需在自身一侧声明任何内容；`static` 是默认值，私有依赖保持私有。

## 图中的选项

`[features]` 是命名的特性束，会在整个依赖图上并集合一（参见 [Profiles 与特性](../reference/profiles.md)）。在 `curl` 依赖上激活 `tls-openssl` 会解析该特性的依赖（openssl port）、注入其 `defines`（注意：`defines` 只作用于该依赖自身的编译单元；要传进你的 TU 需 `public_defines`），并以 `BAKE_CURL_TLS_OPENSSL=1` 编译该依赖；互斥特性（`conflicts`）遵循显式胜默认——显式激活会降级冲突的默认特性（如默认 `tls-mbedtls` 让位于你显式选择的 `tls-openssl`）；两个显式激活互相冲突才报错。要裁剪依赖的默认特性，在该依赖上声明 `default-features = false`——该边只贡献 `features` 显式点名的特性，其他边的并集不受影响（混合时 bake 会警告）。例如只要无 PNG 的 freetype：`freetype = { url = "...", tag = "v2.14.3", default-features = false }`。