# 构建脚本（`build.cpp`）

默认发现覆盖了标准布局。当输入需要逻辑处理——生成的文件列表、非标准目录树、额外 binary——请在包根目录添加 `build.cpp`。它相当于 bake 版本的 Cargo 构建脚本，但方向相反：它不在构建时运行任意命令，而是通过 build.cpp 以声明式方式*描述输入*，其余工作由 bake 的常规流水线完成。

```cpp
// build.cpp
import bake;

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

`build.cpp` 由 bake 自行编译，并针对每个项目重新处理（如同头文件），它会在带有环境变量上下文的情况下执行——`BAKE_SOURCE_DIR`、`BAKE_TARGET`、`BAKE_MOID_NAME`，……结果是构建流水线所使用的声明 JSON。脚本会被缓存：仅当 `build.cpp`、`bake.toml` 或 `bake.build` 模块比缓存的声明更新时，bake 才会重新编译 `build.cpp`。

给编辑器用户的说明：clangd 无法在这些文件中解析 `import bake;`——该诊断是预期的噪声。bake 会自行使用正确的模块路径编译脚本。

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

每个 `b.binary()` 都会成为独立的合成 moid：它会编译自己的源文件，链接主库（以及该库所依赖的全部内容），继承合并后的配置，并且可以 `import` 主 moid 的公开模块。binary 会位于 `out/<triple>/bin/`。

binary 只在被构建的 moid 自身（或 workspace 成员）中产生——**依赖带进来的库不会产出它的 binary**（与 Cargo 一致：依赖的 bin 不构建）。想要某个 port 的 CLI 工具，就把它的仓库当作项目根来构建。

在 `executable` moid 上声明 binary 是错误的——此设计假定存在一个库及其工具。

## `import std;` 快速路径

`build.cpp` 以 C++23 运行，标准库可通过 `import std;` 使用——你可以使用 `std::filesystem`、`std::string`、ranges，或任何需要的内容来处理输入逻辑，而无需依赖 bake 以外的任何内容。