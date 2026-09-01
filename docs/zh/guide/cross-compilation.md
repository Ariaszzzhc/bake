# 交叉编译

bake 可以从任意主机构建到四个 libc 家族——无需组装 sysroot、无需工具链文件、无需外部交叉编译器。

```bash
bake build -t x86_64-linux-musl
bake build -t x86_64-linux-gnu
bake build -t x86_64-windows-gnu
bake build -t aarch64-apple-darwin
```

## 支持的目标

| 目标三元组 | libc | 架构 |
|---|---|---|
| `*-apple-darwin` | 主机 libSystem（macOS），bake 提供 SDK 存根 | aarch64、x86_64 |
| `*-linux-musl` | musl（供应商提供的源代码，从源代码构建） | aarch64、x86_64 |
| `*-linux-gnu` | glibc（源码子集 + 按版本合成的链接存根） | x86_64、aarch64 |
| `*-windows-gnu` | MinGW-w64 v14（供应商提供的源代码） | x86_64、aarch64 |

某个目标的首次构建会从源代码编译供应商提供的 libc 并将其缓存；后续构建与其他所有内容一样是增量式的。`import std;` 和 C++ 模块在每个目标上的工作方式完全相同——`std.pcm` 与 libc++ 对象会按目标在缓存中构建。

## glibc 目标（`*-linux-gnu`）

gnu 目标产物为**动态链接**的 ELF：在目标机的真实 glibc 上运行。默认基线 glibc 2.28，可用三元组版本后缀选择其他版本（≥ 2.28）：

```bash
bake build -t x86_64-linux-gnu          # 默认 2.28
bake build -t x86_64-linux-gnu.2.36     # 显式版本
```

版本后缀同时决定**链接面**与**头文件面**：随附头文件取自最新版 glibc，编译时 `__GLIBC__`/`__GLIBC_MINOR__` 被钉到目标版本，`__GLIBC_PREREQ` 门控随目标开合——`.2.36` 目标可以直接使用 `gettid()`（glibc 2.30 引入）并链接到 `gettid@GLIBC_2.30`，`.2.28` 目标则在编译期就拒绝它。

实现方式：crt 与 `libc_nonshared.a` 从随附的 glibc 源码子集编译；libc 本体从不构建——链接期依据随附的 `abilists` 符号/版本表，为所选版本合成存根 `.so`，靠 `--as-needed` 只保留实际用到的库（hello world 的 `DT_NEEDED` 只有 `libc.so.6`）。静态链接在 gnu 目标上被拒绝（glibc 静态模式 dlopen/NSS 有已知缺陷），静态场景请使用 musl 目标。

## Sanitizer 支持

`bake cc` / `bake c++` 与 `bake build`（`[profile.*] sanitize = [...]`）支持 `-fsanitize=address` 与 `-fsanitize=undefined`：runtime 从随附的 compiler-rt 源码按目标现编（首次链接时，进入全局缓存），各平台采用官方默认形态——linux-gnu / linux-musl 为静态归档（asan 以 whole-archive 链入）；macos 为动态 dylib（`@rpath` 安装名，链接时注入缓存目录 rpath，与官方 clang 布局一致）；windows-gnu 为 ubsan 静态归档 + asan DLL（依上游仅 x86_64，DLL 自动复制到产物旁）。sanitizer 是本机开发工具：只承诺 native 构建（本机实测报告），交叉产物不做验证。其他 sanitizer（tsan、msan 等）未随附：纯编译（`-c`）仍可插桩，链接会被拒绝并给出明确提示。


## 工作原理

所有所需内容均嵌入 bake binary：

1. 从目标三元组解析 libc 家族（`resolve_libc_family`）。
2. 准备运行时：Darwin 使用随附的 `libSystem.tbd` 存根（无需 Xcode）；musl 与 MinGW-w64 CRT + winpthreads 从供应商提供的源代码编译为静态归档文件；gnu 编译 crt/`libc_nonshared.a` 并合成存根库。
3. 使用正确的 sysroot 和驱动风格生成按目标划分的编译/链接命令；在进程内调用 LLD（Darwin、COFF/MinGW 或 GNU 风格）。
4. 按需从 `.def` 文件生成 MinGW 导入库——仅为你实际通过 `-l` 引用的库生成。

musl 与 MinGW 链接优先使用静态归档文件；gnu 目标始终动态链接。

## `bake.toml` 中按目标划分的配置

```toml
[target."*-apple-darwin"]
frameworks = ["Foundation"]

[target."*-linux-musl"]
libraries = ["pthread", "dl"]

[target."*-windows-gnu"]
libraries = ["ws2_32"]
```

当活动目标三元组匹配时，`[target."<glob>"]` 段会生效。glob 基于段进行匹配：`*` 匹配一个完整的、以 `-` 分隔的段，因此 `*-apple-darwin` 匹配 `aarch64-apple-darwin`，而 `*-darwin` 不匹配任何内容。多个段可以匹配同一个目标三元组；它们会合并。

## 运行和测试

交叉构建的 binary 位于 `out/<triple>/bin/`。`bake run` 和 `bake test` 通过 `-t` 选择相同目录，因此在 mac 上：

```bash
bake build -t x86_64-linux-musl    # produces a Linux binary
bake test -t x86_64-linux-musl     # builds for the target; running happens
                                   # only if the host can execute it
```

bake 不提供模拟器；请使用你自己的工具（qemu、wine、Docker）运行外部 binary。

## 输出隔离

每个目标均有自己的子树（`out/<triple>/…`），因此切换目标绝不会使其他目标的增量状态失效——可为主机构建，也可为另外三个目标进行交叉构建，各目录树都会保持预热状态。