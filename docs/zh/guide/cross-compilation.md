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

某个目标的首次构建会从源代码编译自带的 libc 并缓存；后续构建与其他所有内容一样是增量式的。`import std;` 和 C++ 模块在每个目标上的工作方式完全相同——`std` 模块与 libc++ 对象会按目标在缓存中构建。musl 与 windows 目标默认静态链接；gnu 目标始终动态链接。

## glibc 目标（`*-linux-gnu`）

gnu 目标产物为**动态链接**的 ELF：在目标机的真实 glibc 上运行。默认基线 glibc 2.28，可用三元组版本后缀选择其他版本（≥ 2.28）：

```bash
bake build -t x86_64-linux-gnu          # 默认 2.28
bake build -t x86_64-linux-gnu.2.36     # 显式版本
```

版本后缀同时决定**链接面**与**头文件面**：随附头文件取自最新版 glibc，编译时 `__GLIBC__`/`__GLIBC_MINOR__` 被钉到目标版本，`__GLIBC_PREREQ` 门控随目标开合——`.2.36` 目标可以直接使用 `gettid()`（glibc 2.30 引入）并链接到 `gettid@GLIBC_2.30`，`.2.28` 目标则在编译期就拒绝它。

只有实际用到的库会进入 `DT_NEEDED`（hello world 只链 `libc.so.6`）。静态链接在 gnu 目标上被拒绝（glibc 静态模式 dlopen/NSS 有已知缺陷），静态场景请使用 musl 目标。

## Sanitizer 支持

`bake cc` / `bake c++` 与 `bake build`（`[profile.*] sanitize = [...]`）支持 `-fsanitize=address` 与 `-fsanitize=undefined`。runtime 由 bake 自带的 compiler-rt 构建，按目标在首次使用时进入缓存。macos 上 runtime 是动态库，运行时从缓存目录加载——带 sanitizer 的二进制依赖缓存目录存在；linux 上静态链入。windows-gnu 上 ubsan 静态链接，asan 仅 x86_64，运行库自动复制到产物旁。

sanitizer 是本机开发工具：只承诺 native 构建，交叉产物不做验证。其他 sanitizer（tsan、msan 等）不支持——纯编译（`-c`）仍可插桩，链接会被拒绝并给出明确提示。

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