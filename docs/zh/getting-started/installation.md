# 安装

## 要求

从源代码引导构建 bake 需要：

- CMake ≥ 3.30
- Ninja
- 带有 libc++ 的 Clang ≥ 19

这些工具只需使用一次：引导编译器构建 stage 0，随后由 stage 0 构建其余所有内容。生成的 bake binary 不依赖外部工具链——它嵌入 LLVM/Clang/LLD，并内置 libc++、libc++abi、libunwind、compiler-rt、musl 和 MinGW-w64。

## 从源代码构建

```bash
git clone <bake-repo-url> bake
cd bake

# Stage 0: bootstrap with the system Clang
cmake -G Ninja -B build
cmake --build build

# Stage 1: bake builds itself (verifies self-hosting)
./build/bake build
```

stage-0 binary 位于 `build/bake`；自举构建的 binary 位于 `out/<triple>/bin/bake`。二者均具备完整功能。将其中之一加入 `PATH`。

```bash
bake --version
```

## 验证工具链

```bash
bake audit
```

`bake audit` 会验证 bake 的编译器生成 binary 时，除内核接口外，不依赖系统 SDK 的头文件或库。如果需要证明其自包含性，请在全新构建后运行此命令。

## 交叉编译 bake 本身

`bootstrap/build` 可以为任意受支持目标生成 bake binary。它先用 bake 自己的编译器为目标平台交叉编译 LLVM/Clang/LLD，再基于其构建 bake：

```bash
./bootstrap/build x86_64-linux-musl
```

直接 `bake build -t <triple>` 链接的是按宿主机构建的 LLVM 归档；只有在 `BAKE_LLVM_DIR` 指向目标平台 LLVM 安装时才能工作——`bootstrap/build` 会负责准备。

## 打包分发

`bootstrap/package` 会为目标平台构建 bake（release 配置），并按 zig 的布局打
包成自包含归档——binary 加上它运行时需要的一切：

```bash
./bootstrap/package x86_64-linux-musl
```

产物位于 `dist/`，名为 `bake-<arch>-<os>-<version>+<git>.tar.gz`
（windows 为 `.zip`）。解压到任意位置并加入 `PATH` 即可：binary 相对自身
定位 `lib/` 目录，整个解压目录可随意搬动。

## 更新

bake 内置其运行时（libc++、compiler-rt 等）。`scripts/` 下的脚本（`update-runtime.sh`、`fetch-musl.sh`、`fetch-mingw.sh` 等）可更新这些源代码；随后重新构建。
