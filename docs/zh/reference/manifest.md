# 清单格式（`bake.toml`）

本页列出 bake 读取的所有键。未知键会被忽略。

```toml
# Workspace root only: list member directories
[workspace]
members = ["cli", "core"]

[package]
name = "myapp"            # required
version = "0.1.0"
type = "executable"       # executable (default) | lib | dylib

[language]                # defaults: c++17 / c17
cxx = "c++23"             # c++17 | c++20 | c++23
c = "c17"                 # c11 | c17 | c23
# C++20 or later is required for `import std;` and named modules.

[options]                 # bool-only feature flags
use_tls = false
use_json = true

[dependencies]
local = { path = "../local" }                                  # path dep
remote = { url = "https://github.com/org/repo", tag = "v1.0" } # remote dep
flagged = { path = "../x", options = ["use_tls"] }             # activate features

[link]                    # platform-agnostic system linking
libraries = ["z"]         # -l flags

[target."*-apple-darwin"] # applies when the triple matches the glob
frameworks = ["Foundation"]

[target."*-linux-musl"]
libraries = ["pthread", "dl"]

[profile.release]         # override the built-in release profile
opt = 3
lto = true
strip = true

[sources]                 # file extension configuration (defaults shown)
module_ext = [".cppm", ".ixx"]
source_ext = [".cpp", ".cc", ".cxx", ".c"]
header_ext = [".h", ".hpp", ".hxx", ".hh"]
```

## `[package]`

| 键 | 类型 | 默认值 | 含义 |
|---|---|---|---|
| `name` | string | 必填 | moid 名称；决定输出文件名和选项宏 |
| `version` | string | `"0.1.0"` | 近似 SemVer；通过 `BAKE_<NAME>_VERSION` 宏暴露 |
| `type` | string | `"executable"` | `executable` → `out/<t>/bin/`；`lib` → 静态归档 + 公开接口；`dylib` → 共享库 |

## `[language]`

`cxx` 和 `c` 分别设置 C++ 与 C 源文件的语言标准。默认值为 `c++17` / `c17`。（`bake init --std` 会为你写入此表。）

## `[options]`

仅支持 bool 的特性开关。每个选项都会成为设为 `1` 或 `0` 的预处理器宏 `BAKE_<MOID>_<OPTION>`（宏名称转为大写）。选项会在依赖图中按 OR 合并：若任一包启用某个依赖的选项，则该选项会在所有位置启用。命令行覆盖：

```bash
bake build --option use_tls        # =true
bake build --option use_tls=false
```

每个 moid 还会获得版本宏：

```c
BAKE_MYAPP_NAME = "myapp"
BAKE_MYAPP_VERSION = "0.1.0"
BAKE_MYAPP_VERSION_MAJOR = 0
BAKE_MYAPP_VERSION_MINOR = 1
BAKE_MYAPP_VERSION_PATCH = 0
```

## `[dependencies]`

| 键 | 种类 | 含义 |
|---|---|---|
| `path` | 路径依赖 | 包含 `bake.toml` 的相对目录（或不含该文件的供应商源码树——参见[依赖](../guide/dependencies.md)） |
| `url` | 远程 | Git URL |
| `tag` | 远程 | 用于固定版本的标签；解析为锁文件（`bake.lock`）中的提交 |
| `options` | 任意 | 要启用的依赖 `[options]` 列表 |

## `[link]` 和 `[target."…"]`

`[link]` 在所有平台适用；仅当活动目标三元组与 glob 匹配时，`[target."<glob>"]` 节才适用（`*` 匹配一个完整分段）。两者均接受：

- `libraries` — 要链接的系统库（`-l`）
- `frameworks` — macOS framework

匹配的节会与 `[link]` 及彼此合并。

## `[profile.<name>]`

完整字段列表与内置默认值参见[Profile 和选项](profiles.md)。

## `[sources]`

默认发现用于分类文件的扩展名列表：

- `module_ext` — C++ 模块接口（`.cppm`、`.ixx`）
- `source_ext` — 普通编译单元（`.cpp`、`.cc`、`.cxx`、`.c`）
- `header_ext` — 头文件，供工具/IDE 使用；永不编译

一个包就是一个 *moid*——一个 `bake.toml` 描述一组编译单元。多个包位于一个[工作区](../guide/workspaces.md)中。
