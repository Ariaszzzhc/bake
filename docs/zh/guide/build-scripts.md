# 可编程输入（`build.cpp`）

`bake.toml` 定义项目及其构建配置。默认发现覆盖标准源码布局。当输入选择需要逻辑处理时，可以在包根目录添加 `build.cpp`，描述源码列表、非标准源码目录、额外 binary 和测试。bake 将这些声明与清单合并，再构建由此生成的图。

```cpp
// build.cpp
import bake.build;

int main() {
    bake::Builder b;

    b.sources({"src/*.cpp", "src/detail/*.cpp"});
    b.public_modules("public/*.cppm");
    b.public_headers("include");

    return b.build();
}
```

## 三个输入档位

包恰好在三个档位之一运行，由现有内容和脚本声明的内容决定：

1. **默认发现**——没有 `build.cpp`。输入来自标准的 `src/` + `public/` 布局（参见[起步](../getting-started/first-steps.md)）。
2. **增量**——一个未声明任何主 moid 输入的 `build.cpp`（没有 `sources`、`public_modules`、`public_headers`）。主 moid 仍然使用默认发现；脚本只提供额外内容：binary、测试、预构建库。
3. **自定义**——一个声明主 moid 输入的 `build.cpp`。发现会被完全替换：只编译你声明的内容，不编译其他内容。

档位选择由数据驱动：运行脚本后，`sources.empty() && public_include_dirs.empty()` ⇒ 增量；否则为自定义。

## build.cpp 能做与不能做的事

**能做**——描述输入：

- moid 编译哪些源文件和模块接口，以及其中哪些是公开的
- include 目录（私有和公开）
- 要链接的预构建库（`.a`/`.lib` 路径）
- 额外 binary——由包构建的附加可执行文件（见下文）
- 测试注册（参见[测试](testing.md)）

**不能做**——更改配置。编译器标志、define、系统库、framework、优化：这些应置于 `bake.toml` 中（`[profile.*]`、`[link]`、`[target.*]`、`[features]`）。build.cpp 不能重命名 moid、改变其类型，也不能覆盖清单。构建脚本负责构建*什么*；清单负责构建*如何进行*。

这种划分是有意为之的：输入发现可以任意适配项目，而配置保持声明式、可进行差异比较且受目标条件控制。

## 脚本如何运行

项目声明需要准备时，bake 会自行编译并执行 `build.cpp`，通过 `BAKE_SOURCE_DIR`、`BAKE_TARGET`、`BAKE_MOID_NAME` 等环境变量提供上下文。结果是构建流水线所使用的声明 JSON。共享项目快照记录相关 `build.cpp`、每个 workspace/member `bake.toml` 与实际 workspace `bake.lock` 的精确存在状态和文件系统 mtime；任一不匹配都会重新准备声明。

bake 在编译 `build.cpp` 时提供 `bake.build` 的模块路径。CLI 命令会将程序执行产生的 stdout 和 stderr 转发到对应输出流。复用缓存声明时不会重新执行程序，也不会重放此前的输出。

完整的 `Builder` API 见 [build.cpp API reference](../reference/builder-api.md)。

## 额外 binary

`lib`/`dylib` moid 可以附带伴随可执行文件——CLI 工具、测试工具——它们链接该库：

```cpp
bake::Builder b;

b.public_modules("public/*.cppm");

b.binary("mytool")
     .sources("tools/main.cpp")
     .include_dirs("tools");

return b.build();
```

每个 `b.binary()` 都会成为独立的合成 moid：它会编译自己的源文件，链接主库（以及该库所依赖的全部内容），继承合并后的配置，并且可以 `import` 主 moid 的公开模块。binary 位于 `out/<full-target>/<profile>/bin/`。

binary 只在被构建的 moid 自身（或 workspace 成员）中产生——**依赖带进来的库不会产出它的 binary**（与 Cargo 一致：依赖的 bin 不构建）。想要某个 port 的 CLI 工具，就把它的仓库当作项目根来构建。

在 `executable` moid 上声明 binary 是错误的——此设计假定存在一个库及其工具。

## `import std;` 快速路径

`build.cpp` 以 C++23 运行，标准库可通过 `import std;` 使用——你可以使用 `std::filesystem`、`std::string`、ranges，或任何需要的内容来处理输入逻辑，而无需依赖 bake 以外的任何内容。
