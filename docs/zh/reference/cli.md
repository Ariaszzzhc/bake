# 命令行接口

```
bake <subcommand> [options]
bake <subcommand> --help
```

## 子命令

| 命令 | 用途 |
|---|---|
| `bake init [name]` | 创建项目脚手架 |
| `bake build` | 构建项目（或工作区） |
| `bake run [args…]` | 构建后运行可执行文件；末尾参数传递给程序 |
| `bake test [name]` | 构建并运行已注册的测试 |
| `bake add <url> [--tag <tag> \| --branch <branch> \| --rev <rev>] [name] [--target <glob>]` | 添加 Git 或归档依赖 |
| `bake remove <name> [--target <glob>]` | 删除依赖声明 |
| `bake update [dep]` | 重新解析一个依赖；省略时重新解析全部依赖 |
| `bake clean` | 删除 `out/` |
| `bake cc` / `bake c++` / `bake ar` | 直接调用内嵌的 Clang/LLVM 工具（`zig cc` 风格） |
| `bake audit` | 验证工具链的自包含性 |

## 全局选项

| 标志 | 含义 |
|---|---|
| `-V`, `--version` | 打印版本 |
| `-h`, `--help` | 打印帮助信息 |

## 构建选项（`build`、`run`、`test`）

| 标志 | 含义 |
|---|---|
| `--option <name>[=value]` | 覆盖 `bake.toml` 中的 `[options]` 值 |
| `-t <triple>`, `--target=<triple>` | 交叉编译目标 |
| `--release` | 使用 release profile 构建 |
| `--profile <name>` | 使用指定 profile 构建 |
| `-p <member>` | 构建、测试或运行指定的工作区成员 |
| `-j <n>` | 并行任务数 |
| `-v`, `--verbose` | 逐文件编译进度 |
| `--locked` | 若 `bake.lock` 缺失或已过期则失败 |
| `--offline` | 永不连接网络 |
| `--frozen` | `--locked` + `--offline` |

## `bake init`

```
bake init [name] [--type <executable|lib|dylib>] [--std <standard>]
```

- `--type` — moid 类型，默认为 `executable`。
- `--std` — `c11|c17|c23|c++17|c++20|c++23`，默认为 `c++20`；C 标准会生成包含 C 源文件的脚手架。
- `name` 可以是路径：项目在该路径创建，名字取最后一段（`bake init tools/hello` → `tools/hello/`，`name = "hello"`）。项目名必须是可移植文件名——`[A-Za-z0-9][A-Za-z0-9._+@-]*`，不能以点结尾，不能用 `CON` 这类保留设备名。
- 未指定 `name` 时，在当前目录中生成脚手架。
- 创建 `bake.toml`、`src/`、`public/`、`tests/`、`.gitignore`；`lib` 脚手架还会添加 `public/<name>/<name>.hpp`。

## `bake add`

```
bake add <url> [--tag <tag> | --branch <branch> | --rev <rev>] [name] [--target <glob>]
```

- `--tag`、`--branch` 和 `--rev` 选择 Git ref，且三者互斥。
- 以受支持归档后缀结尾的 URL 会自动作为直接归档依赖添加；归档依赖不能带 ref。
- `[name]` 覆盖从 URL 推导的名称。绝对本地路径会规范化为 `file://` URL。
- `--target <glob>` 写入 `[target."<glob>".dependencies]`；非法 glob 会立即被拒绝。
- 在所选作用域中，完全相同的已有声明会被跳过，并提示运行 `bake update`；不同声明会被替换。对 manifest 的修改会保留注释。

## `bake remove`

```
bake remove <name> [--target <glob>]
```

- 使用 `--target` 时，从 `[target."<glob>".dependencies]` 删除声明。
- 未指定时，先检查 `[dependencies]`，再检查目标依赖表。若名称出现在多个作用域，bake 会列出匹配作用域并要求 `--target`。
- 删除不再被任何依赖作用域引用的锁条目。已下载内容保留在缓存中。

## `bake update`

```
bake update [dep]
```

- `bake update <dep>` 只强制解析该依赖的 URL；其他依赖未变的条目会被原样搬运。
- 不带名称的 `bake update` 重新解析全部依赖。它是唯一可能移动所有已锁定 ref 的命令。

## `bake test`

```
bake test [name] [--all] [-p <member>] [-j <n>]
```

- 无参数：运行默认注册的测试（若不存在则报错）。
- `name`：运行该测试。
- `--all`：运行所有已注册的测试。
- 测试 binary 的输出会原样传递；任一选定测试失败时，退出码非零。

## `bake run`

构建后运行所选内容的唯一可执行文件。工作区中存在多个可执行文件时，bake 会列出候选项并要求指定 `-p`。末尾位置参数会转发给程序。

## 退出码

成功时为 0；任何失败时为 1（构建错误、测试失败、无法满足的 `--locked`/`--offline`、缺少 manifest）。测试失败会令 `bake test` 以非零退出状态退出，并保留失败 binary 的退出码语义。
