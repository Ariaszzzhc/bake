# bake 手册

bake 是一体化的 C/C++ 构建系统和编译器工具链。可以将其理解为“面向 C++ 的 Cargo”与 `zig cc` 的结合：默认源文件发现、可编程的 `build.cpp` 输入声明、包管理，以及集成式 LLVM/Clang 工具链——全部集成于一个 binary 中，且无需任何外部依赖。

- 无需 Makefile、无需 CMakeLists，也无需生成步骤。`bake build` 即是完整的流水线。
- 编译器嵌入于其中。bake 将 LLVM、Clang 和 LLD 作为库链接，并在进程内编译——不存在编译器子进程，也不要求系统工具链。
- C++20 模块（以及 `import std;`）开箱即用，包括交叉编译。

本书的组织方式与 Cargo Book 相同：

**入门指南**

- [安装](getting-started/installation.md)
- [第一步](getting-started/first-steps.md)——初始化、构建、运行与项目布局

**指南**

- [依赖](guide/dependencies.md)——路径依赖、远程依赖与锁文件(bake.lock)
- [工作区](guide/workspaces.md)
- [构建脚本](guide/build-scripts.md)——`build.cpp` 与三档输入
- [测试](guide/testing.md)——`add_test`、`bake test`
- [交叉编译](guide/cross-compilation.md)

**参考**

- [清单格式](reference/manifest.md)——每个 `bake.toml` 键
- [Profile 与选项](reference/profiles.md)
- [build.cpp API](reference/builder-api.md)——`bake::Builder` 类
- [命令行界面](reference/cli.md)
- [输出与缓存布局](reference/output-and-cache.md)
