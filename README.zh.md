# bake

**一站式 C/C++ 构建系统与编译器工具链。**

一个二进制，一条命令。不需要系统 clang，不需要系统 libc++，不需要系统链接器，不需要 Makefile。

## bake 是什么

bake 同时是构建工具、包管理器和完整的编译器——打包在一个可执行文件里。它用内嵌的 LLVM/Clang 编译 C/C++ 源码，用内嵌的 LLD 链接，自带 libc++、libc++abi、libunwind、compiler-rt 和 libc。结果是：一个在任何机器上都完全一致的构建环境，零外部依赖。

### 愿景

C/C++ 的构建应该像 Rust 的 `cargo build` 一样简单。你写代码，运行一条命令，拿到二进制。不需要找编译器版本，不需要折腾 CMake 预设，不需要 `brew install llvm`，不存在"在我机器上能跑"的问题。

bake 让这件事变成了现实。每次安装 bake 都自带相同的编译器、相同的标准库、相同的 libc——不管主机装了什么。交叉编译到 macOS、Linux、Windows 开箱即用，从任何主机都能做。

### 灵感来源

- **Cargo**（Rust）——约定式构建、workspace 模型、包管理、lockfile。
- **zig cc**——以单一二进制分发自带 libc 的 C/C++ 编译器，能从任何主机交叉编译到任何目标。

bake 结合了两者：Cargo 的人体工学 + zig cc 的自包含工具链，服务于 C 和 C++。

## 支持平台

### 主机（bake 运行环境）

| 系统 | 架构 |
|---|---|
| macOS | aarch64（Apple Silicon）、x86_64（Intel） |
| Linux | aarch64、x86_64 |
| Windows | x86_64 |

### 交叉编译目标

| 目标 triple | libc | 架构 |
|---|---|---|
| `aarch64-darwin`、`x86_64-darwin` | 宿主 libSystem（macOS） | aarch64、x86_64 |
| `aarch64-linux-musl`、`x86_64-linux-musl` | musl（自带源码） | aarch64、x86_64 |
| `x86_64-windows-gnu`、`aarch64-windows-gnu` | MinGW-w64 v14（自带源码） | x86_64、aarch64 |

三个 libc 家族都是自包含的——不需要 SDK，不需要 sysroot，不需要 NDK。

## 功能

### 约定式构建

在 `src/` 旁边放一个 `bake.toml`，bake 自动发现源码、编译、链接。不需要任何构建脚本。

### `build.cpp` 逃生舱

约定不够用时，写一个 `build.cpp`：

```cpp
import bake.build;

int main() {
    bake::Builder builder;
    builder.executable("myapp")
        .sources("src/*.cpp")
        .sources("src/platform/*.cpp", {
            .defines = {"PLATFORM_LINUX"}
        })
        .include_dirs("include")
        .std("c++23");
    return builder.build();
}
```

bake 在 configure 阶段编译并执行这个脚本。两条路径产出相同的内部表示——没有二等公民。

### 交叉编译

```bash
bake build -t x86_64-linux-musl
bake build -t x86_64-windows-gnu
```

不需要安装额外工具链。bake 自带 musl 和 MinGW-w64 源码，按需从源码构建 CRT。

### 包管理

```toml
[dependencies]
curl = { url = "https://github.com/curl/curl", tag = "curl-8_21_0" }
```

`bake update` 把 tag 解析为 commit、下载源码、锁定版本。依赖不需要是 bake 项目——一个 `build.cpp` 包装就能消费任何 C/C++ 库。

### `import std` 支持

完整支持 C++23 模块，包括 `import std;`。bake 从自带的 libc++ 源码预构建 `std.pcm` 并全局缓存。

### 增量构建

基于内容的指纹——改一个源文件只重跑受影响的动作。只 touch 不改内容则什么都不重建。

## 快速上手

### 从源码构建 bake（Stage 0）

依赖：CMake ≥ 3.30、Ninja、Clang ≥ 19 + libc++。

```bash
git clone --recursive https://github.com/arias/bake-compiler.git
cd bake-compiler
cmake -G Ninja -B build && cmake --build build
```

### 自举（Stage 1）

bake 用自己的集成编译器构建自身：

```bash
./build/bake build
```

`out/bin/bake` 完全自包含——不依赖任何系统工具链。

### 创建项目

```bash
bake init myapp
cd myapp
bake build
./out/bin/myapp
```

## 命令

```
bake init [name]           创建项目脚手架
bake build                 构建项目
bake run                   构建并运行默认可执行文件
bake clean                 删除 out/
bake add <url> --tag <t>   添加远程依赖
bake update                重新解析 tag 为 commit，更新 lock
bake test                  构建并运行测试（如果配置了）
```

构建标志：`-t <triple>`（交叉编译目标）、`-j <n>`（并行度）、`-p <member>`（workspace 成员）、`-v`（详细输出）、`--locked`、`--offline`、`--frozen`。

---

[English](README.md)
