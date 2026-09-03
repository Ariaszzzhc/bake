# `build.cpp` API（`bake::Builder`）

`build.cpp` 导入 `bake` 模块，并通过 `bake::Builder` 驱动构建：

```cpp
import bake;

int main() {
    bake::Builder b;
    // … describe inputs …
    return b.build();
}
```

脚本负责**输入**；`bake.toml` 负责配置（标志、定义、库）。模式是相对于包根目录的 glob（`*`、`**`、`?`）；不含 glob 的路径必须存在。

## 输入方法

所有输入方法均可链式调用，并接受单个模式或花括号列表。

```cpp
b.sources({"src/*.cpp", "gen/output.cpp"}); // private translation units
b.public_modules("public/*.cppm");          // exported module interfaces
b.include_dirs("third_party/include");      // private include path
b.public_headers("include");                // include path exposed to consumers
b.prebuilt_lib("vendor/libz.a");            // prebuilt archive to link
```

| 方法 | 效果 |
|---|---|
| `sources(patterns)` | 编译为私有源文件。非公开模块接口也属于此处（例如 `src/*.cppm`）。 |
| `public_modules(patterns)` | 编译为公开模块接口——依赖方可以 `import` 它们。接口身份来自声明而非文件扩展名：可指名任意 C++ 源（上游模块单元未必用 `.cppm`/`.ixx`，如 fmt 的 `src/fmt.cc`）。 |
| `include_dirs(dirs)` | 私有包含目录（仅此 moid）。 |
| `public_headers(dirs)` | 公开包含目录（传播给使用方）。 |
| `prebuilt_lib(path)` | 将预构建静态归档链接进 moid。 |

## 额外 binary

`b.binary(name)` 返回一个 `BinaryBuilder`，用于描述一个额外的可执行文件，该文件会链接 moid 的库和依赖：

```cpp
b.binary("mytool")
    .sources("tools/main.cpp")
    .include_dirs("tools");
```

binary 源文件可以 `import` 主 moid 的公开模块，并使用其公开头文件。仅当 moid 类型为 `lib` 或 `dylib` 时才允许 binary。

## 测试

```cpp
b.add_test("suite-name", "binary-name").set_default();
```

注册由一个 binary（通过 `binary()` 声明）支持的具名测试。`set_default()` 将该测试标记为 `bake test` 默认运行的测试；后续调用会清除先前的默认标记。参见[测试](../guide/testing.md)。

## 上下文访问器

| 成员 | 值 |
|---|---|
| `b.feature("name")` | `[features]` 特性是否激活（图上并集合一） |
| `b.source_dir()` | 包根目录（绝对路径） |
| `b.build_dir()` | `out/` 目录（绝对路径） |
| `b.target()` | 活动目标三元组——原生构建也始终携带宿主三元组（如 `aarch64-apple-darwin`），永不为空；跨平台源选择可直接按它分支 |
| `b.dep_src_dir("alias")` | 已解析依赖的源目录（若未知则为空）——可用于 glob 依赖的头文件 |

## 执行模型

- 脚本会由 bake 按 C++23 编译并缓存；仅在 `build.cpp`、`bake.toml` 或 `bake.build` 模块发生变化时重新编译。
- 上下文通过环境变量传入（`BAKE_SOURCE_DIR`、`BAKE_TARGET`、`BAKE_OPTIONS`、`BAKE_DEPS`、…）——构造函数会读取它们；通常无需接触它们。
- `b.build()` 会写入声明，并在成功时返回 0。返回其他值会使构建失败，同时显示脚本的 stderr。
- 档位选择由数据驱动：若脚本没有声明主 moid 输入，默认发现仍处于活动状态（增量档位）；任何 `sources`/`public_modules`/`public_headers` 调用都会切换为完全自定义输入。参见[构建脚本](../guide/build-scripts.md)。
- 编辑器提示：clangd 会为 `build.cpp` 报告 `Module 'bake' not found`——这是预期行为；bake 会自行编译该脚本。
