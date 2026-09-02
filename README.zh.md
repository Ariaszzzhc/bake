# bake

**C/C++ 构建系统、包管理器、编译器工具链——一个二进制。**

写代码，跑 `bake build`，拿二进制。不需要 Makefile，不需要 CMakeLists，
不需要 `brew install llvm`，不需要搭 sysroot，也不存在"在我机器上能跑"。

[English](README.md)

## 这是什么

bake 把三样东西焊在一个可执行文件里：

- **构建系统。**约定优于配置：`src/` 旁边放一个 `bake.toml` 就是完整的构建
  定义。约定不够用时，写一个 `build.cpp` 描述输入——用真正的 C++，而不是
  又一门 DSL。
- **包管理器。**Git 依赖、归档依赖、路径依赖、workspace、lockfile，以及
  给 CI 用的 `--frozen`。
- **编译器工具链。**bake 内嵌 LLVM、Clang、LLD，自带 libc++、compiler-rt、
  musl、MinGW-w64 和 glibc 链接面。编译和链接都在进程内完成：没有编译器
  子进程，永远不碰系统工具链。

为什么要做这个？因为在 C/C++ 的世界里，工具链本身就是环境噪音——你拿到
哪个 clang，取决于机器、包管理器和当天月相。bake 把整个工具链钉死在构建
工具内部：在你机器上能构建，就能在所有机器上构建；交叉编译是一个旗标，
不是一个工程。

灵感来源短而诚实：[Cargo] 的使用体验——约定、workspace、lockfile——加上
[zig cc] 证明过的那件事：编译器可以是单个自包含二进制，通吃所有目标平台。

[Cargo]: https://doc.rust-lang.org/cargo/
[zig cc]: https://ziglang.org/download/#zig-cc

## 一分钟尝鲜

```
$ bake init hello && cd hello
$ bake build
   Building hello v0.1.0
   Compiling hello v0.1.0
    Finished in 1.1s
$ bake run
Hello from hello!
```

交叉编译只是同一命令加个目标：

```
$ bake build -t x86_64-linux-musl
   Building hello v0.1.0
   Compiling hello v0.1.0
    Finished in 1.1s
$ file out/x86_64-linux-musl/bin/hello
out/x86_64-linux-musl/bin/hello: ELF 64-bit LSB executable, x86-64, ... statically linked
```

没有工具链文件，没有 SDK，没有 `-DCMAKE_TOOLCHAIN_FILE`。新目标的首次构建
会从自带源码编译该目标的 C 运行时并缓存，之后全部增量。

C++20 模块和 `import std;` 开箱即用——bake 用自带的 libc++ 预构建 `std`
模块，按目标缓存：

```cpp
import std;
import math;

int main() {
    std::println("{}", triple(14));    // 42
}
```

默认发现不够用时，`build.cpp` 说了算：

```cpp
import bake;

int main() {
    bake::Builder b;
    b.sources({"src/*.cpp", "src/platform/*.cpp"});
    b.public_headers("include");
    return b.build();
}
```

脚本只描述**输入**；配置（profile、旗标、链接库、按目标设置）仍然归
`bake.toml` 管。完整故事见[文档](docs/)。

## 支持平台

主机：macOS（aarch64、x86_64）、Linux（aarch64、x86_64）、Windows（x86_64）。

交叉编译目标，任意主机到任意目标：

| 目标 triple | libc | 架构 |
|---|---|---|
| `*-apple-darwin` | 宿主 libSystem（bake 随附 SDK 存根） | aarch64、x86_64 |
| `*-linux-musl` | musl，从自带源码构建 | aarch64、x86_64 |
| `*-linux-gnu` | glibc：自带头文件 + 合成链接存根 | x86_64、aarch64 |
| `*-windows-gnu` | MinGW-w64 v14，从自带源码构建 | x86_64、aarch64 |

gnu 目标用 triple 后缀选基线（`-t x86_64-linux-gnu.2.36`），运行时链接真
glibc；其余目标默认静态链接。

同样在车上：全目标 `import std;` 与具名模块、原生构建的 asan/ubsan、三种
对象格式的进程内 LLD，以及想直接用内嵌工具链时的 `bake cc` / `bake c++` /
`bake ar`（`zig cc` 风格）。

## 获取 bake

还没有二进制发布——自己编一次（用任意系统 Clang）：

```bash
git clone --recursive <bake-repo-url> bake
cd bake

# Stage 0：用系统编译器自举。
# 需要 CMake >= 3.30、Ninja、Clang >= 19（libc++）。
cmake -G Ninja -B build && cmake --build build

# Stage 1：bake 构建自己。
./build/bake build
```

`out/<host-triple>/bin/bake` 就是自举产物——从这以后它只向系统要内核。
跑一下 `bake audit`，它会验证构建确实是自包含的。

交叉编译 bake 本体：`./bootstrap/build x86_64-linux-musl`——它会先用 bake
自己的编译器为目标平台重建 LLVM。

打包分发（release 二进制 + 全部 vendored 运行时，zig 式布局，解压即用）：
`./bootstrap/package x86_64-linux-musl`——产物在 `dist/`。

## 命令

```
bake init [name]     创建项目脚手架
bake build           构建（增量、并行）
bake run [args…]     需要时构建，然后运行
bake test [name]     构建并运行注册的测试
bake add <url>       添加依赖（--tag / --branch / --rev）
bake remove <name>   移除依赖
bake update [dep]    重新解析锁定的 ref（唯一会移动它们的命令）
bake clean           删除 out/
bake cc / c++ / ar   直接使用内嵌工具链
bake audit           验证工具链自包含性
```

常用旗标：`-t <triple>`、`--release` / `--profile <name>`、`-p <member>`、
`-j <n>`、`--option <name>[=value]`、`--locked`、`--offline`、`--frozen`。
完整参考见文档。

## 文档

bake 附带双语使用手册：

- [English](docs/en/README.md)
- [中文](docs/zh/README.md)

从[第一步](docs/zh/getting-started/first-steps.md)开始，然后看
[依赖](docs/zh/guide/dependencies.md)、
[交叉编译](docs/zh/guide/cross-compilation.md)和
[build.cpp](docs/zh/guide/build-scripts.md)。

## 许可证

[MIT](LICENSE)。`lib/` 与 `third_party/` 下的自带源码保留各自上游许可证
（LLVM、Darwin、musl、MinGW-w64、glibc、compiler-rt、nlohmann/json、toml++）。
