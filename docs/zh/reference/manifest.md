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

[features]                # 特性束：可选依赖 / defines / 互斥 / 平台
default = ["tls", "zlib"]
tls = { dependencies = { mbedtls = { url = "...", tag = "v3.6.7" } }, defines = ["USE_MBEDTLS"] }
zlib = {}

[dependencies]
local = { path = "../local" }                                      # 路径依赖
by_tag = { url = "https://github.com/org/repo", tag = "v1.0" }   # Git 标签
by_branch = { url = "https://github.com/org/repo", branch = "main" } # Git 分支
by_rev = { url = "https://github.com/org/repo", rev = "d5598e..." }  # 确切 Git revision
head = { url = "https://github.com/org/head" }                     # 默认分支 HEAD
archive = { url = "https://example.com/lib-1.0.tar.xz" }           # 直接归档
flagged = { path = "../x", features = ["use-tls"] }              # 激活特性

[link]                    # platform-agnostic system linking
libraries = ["z"]         # -l flags

[target."*-apple-darwin"] # applies when the triple matches the glob
frameworks = ["Foundation"]

[target."*-linux-musl"]
libraries = ["pthread", "dl"]
defines = ["Z_HAVE_UNISTD_H"]   # upstream feature macros (target tables only)

[target."*-apple-darwin".dependencies]
metal_cpp = { url = "https://example.com/metal-cpp-1.0.0.zip" }

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

## `[features]`

命名的特性束。每个特性可携带四类东西：

```toml
[features]
default = ["tls-mbedtls", "zlib"]     # 保留名：无任何激活时的默认激活集

tls-mbedtls = {
  platforms = ["*-apple-darwin", "*-linux-*"],   # 仅这些目标生效；缺省 = 全部
  dependencies = { mbedtls = { url = "...", tag = "v3.6.7" } },  # 条目语法与 [dependencies] 相同，仅激活时解析
  defines = ["USE_MBEDTLS"],                     # 注入本包编译的宏（"NAME" 或 "NAME=VALUE"），默认私有
  public_defines = ["PCRE2_CODE_UNIT_WIDTH=8"],  # 同时传播给依赖方 TU（如头文件要求的静态链接/形态宏）
  conflicts = ["tls-openssl"],                   # 互斥特性名
}
zlib = {}                             # 纯宏特性：只产生 BAKE_<MOID>_ZLIB
```

激活语义：

- **并集合一**：包的有效集 = 自身 `default` ∪ 所有入边激活 ∪ CLI。`build.cpp` 用 `b.feature("zlib")` 查询
- 每个声明的特性都产生宏 `BAKE_<MOID>_<FEATURE>`，激活为 `1`、未激活为 `0`
- **conflicts** 遵循显式胜默认：与显式激活（依赖边/CLI）冲突的默认特性被静默降级（打印一条提示，其依赖与 defines 随之消失）——消费者可以替换包的默认后端；两个显式激活或两个默认特性互相冲突才在 configure 期报错，并点名两条激活来源
- **platforms** 是生效白名单：默认集里不匹配当前目标的特性静默不贡献；显式激活（依赖边或 `--feature`）不匹配则报错
- 不激活的特性，其依赖不进图、不进锁、不下载

命令行激活（仅 root moid）：

```bash
bake build --feature tls-openssl
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

`[dependencies]` 中的每个 alias 声明一个依赖。`[target."<glob>".dependencies]` 使用相同的条目语法。

| 键 | 种类 | 含义 |
|---|---|---|
| `path` | 路径依赖 | 本地目录。每次构建都从磁盘解析，且永不写入 `bake.lock`。 |
| `url` | Git 或归档依赖 | Git URL，除非其以 `.tar.gz`、`.tgz`、`.tar.bz2`、`.tbz2`、`.tar.xz`、`.txz` 或 `.zip` 结尾；这些后缀声明直接归档依赖。 |
| `tag` | Git ref | 选择标签。与 `branch`、`rev` 互斥。 |
| `branch` | Git ref | 选择分支。与 `tag`、`rev` 互斥。 |
| `rev` | Git ref | 选择确切的 Git revision。与 `tag`、`branch` 互斥。 |
| `features` | 任意依赖 | 要在该依赖包上激活的 `[features]` 列表。 |
| `default-features` | 任意依赖 | `false` 时该边不再贡献依赖包的 `default` 特性集——仅 `features` 显式点名的特性激活。并集语义：其他边（或将该包作为根构建）仍会贡献默认特性；混合时 bake 会警告。 |

不带 `tag`、`branch` 或 `rev` 的 Git 依赖会在构建时解析其默认分支 `HEAD`。归档依赖不得指定 ref。tar 归档由 `tar` 提取；ZIP 归档使用 `unzip`。

## `[target."<glob>".dependencies]`

目标依赖表使用与 `[dependencies]` 相同的 alias 条目。对某个构建目标，bake 使用全局依赖与 glob 匹配当前目标三元组的每个目标依赖表的并集。锁文件覆盖所有作用域的并集，而图解析只使用当前目标的有效集。

不要在两个作用域中以不同方式定义同一 alias：bake 会报错并点名两个冲突的表。命令与解析行为参见[依赖](../guide/dependencies.md)。

## `bake.lock`

锁文件记录所有非路径依赖。Git 键为 `git:<url>@<commit>`，归档键为 `archive:<url>`。每个条目包含 `url`、`ref`、`ref_type`、`commit` 和 `integrity`；可选的 `name` 记录解析出的原生包的 `[package].name`。增量解析会原样搬运 URL/ref 相同的条目，不会再次远程查询或下载。

## `[link]` 和 `[target."…"]`

`[link]` 在所有平台适用；仅当活动目标三元组与 glob 匹配时，`[target."<glob>"]` 节才适用（`*` 匹配一个完整分段）。两者均接受：

- `libraries` — 要链接的系统库（`-l`）
- `frameworks` — macOS framework

`[target."<glob>"]` 节额外接受：

- `defines` — 本包编译时传入的预处理器宏（`"NAME"` 或 `"NAME=VALUE"`），默认私有
- `public_defines` — 同 `defines`，但会传递性传播给依赖方的编译单元（静态链接宏如 `CARES_STATICLIB`、头文件形态宏如 `PCRE2_CODE_UNIT_WIDTH=8`）
- `flags` — 追加到编译命令的 flags
- `include_dirs` — 追加的 include 路径

匹配的节会与 `[link]` 及彼此合并。目标表是把上游库的平台差异（configure 本该探测的特性宏）声明化的地方——例如 zlib 的移植在非 Windows 目标上只需 `defines = ["Z_HAVE_UNISTD_H"]`。

## `[profile.<name>]`

完整字段列表与内置默认值参见[Profile 和选项](profiles.md)。

## `[sources]`

默认发现用于分类文件的扩展名列表：

- `module_ext` — C++ 模块接口（`.cppm`、`.ixx`）
- `source_ext` — 普通编译单元（`.cpp`、`.cc`、`.cxx`、`.c`）
- `header_ext` — 头文件，供工具/IDE 使用；永不编译

一个包就是一个 *moid*——一个 `bake.toml` 描述一组编译单元。多个包位于一个[工作区](../guide/workspaces.md)中。
