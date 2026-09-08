# 输出与缓存布局

## 项目输出：`out/`

所有构建产物均位于项目根目录下，并按完整目标三元组与 profile 组合划分子树：

```
out/
├── aarch64-apple-darwin/      # 示例中的本机目标
│   ├── dev/
│   │   ├── bin/               # 可执行文件（主目标 + build.cpp 声明的 binaries）
│   │   ├── lib/               # lib → .a，dylib → .dylib/.so/.dll
│   │   ├── .obj/              # 对象文件
│   │   ├── .bmi/              # 模块 PCM 文件
│   │   └── .bake/             # 项目状态、动作收据、LSP 数据
│   └── release/               # 独立的 release 上下文
└── ...
```

- `bin/` 存放 moid 的可执行文件，以及每个 `b.binary()` 目标。
- `lib/` 存放 lib 的 `lib<name>.a` 以及 dylib 的共享库。依赖方会链接该归档，并接收其公开头文件和模块。
- 目标/profile 上下文内的 `.bake/` 存放共享项目快照、构建动作收据与分析数据。
  Core 状态隔离在 `.bake/core/<context-key>/` 下：不可变快照位于
  `snapshots/<sha256>.json`，带摘要保护的最近一次成功观察为 `latest.json`，
  已完成请求记录位于 `results/<request-id>.json`，进行中的工作暂存在 `work/`。
  同一上下文中的 `.bake/build-publication.lock` 串行化构建发布。

`bake clean` 会完全删除 `out/`。

脏构建动作先写入当前请求的 Core `work/` 目录。发布前，bake 获取目标/profile
发布锁并重新哈希已捕获的强输入；输入仍匹配时，规划产物才会原子替换目标路径，
最后发布不可变的 ready 结果。只有成功请求记录以及每个现有产物的摘要与大小都
仍然匹配，缓存动作才可复用。

`run` 或 `test` 启动生成的可执行文件前，bake 会验证构建结果已成功发布，然后读取
并捕获所选可执行文件及其依赖闭包内由项目生成的共享库。每个捕获文件的摘要
与大小都必须匹配构建结果。进程从这份独立的 `bin`/`lib` 布局运行，因此并发替换
规范输出不会改变本次运行；进程退出后，bake 会删除这份捕获。预构建共享库与外部
共享库不在捕获范围内。

生成的 dylib 在项目布局内使用稳定的运行时相对身份。在 macOS 上，默认身份为
`@rpath/<filename>`，使用方采用 `@loader_path/../lib`；在 ELF 目标上，默认 SONAME
为 `<filename>`，使用方采用 `$ORIGIN/../lib`。显式 install name 或 SONAME 会保留。
在 Windows 上，生成的 DLL 依赖会捕获到可执行文件旁边。

语言服务器把当前上下文的分析产物放在项目内部，与其他构建中间产物同处一地。
`out/<full-target>/<profile>/.bake/lsp-preamble/` 存放内容寻址的 preamble PCH
及其 JSON 收据。PCH 对象身份覆盖 Core 上下文、分析命令与编译器、精确的
preamble 前缀以及已捕获的依赖身份；复用前，其收据还会重新验证上下文、依赖、
前缀、边界和产物路径。
`out/<full-target>/<profile>/.bake/lsp-index/` 按编译单元存放不可变 JSON
符号 shard；每个 shard 的键覆盖编译描述、编译器、已捕获的源码身份与依赖身份，
格式错误或身份不匹配的 shard 会被拒绝并重建。两处存储都位于 `out/` 下，
因此 `bake clean` 会连同其他产物一并清理，临时项目也会带走自己的分析产物。

## 全局缓存：`~/.cache/bake/`

在各项目之间共享，按目标与内容为键：

| 路径 | 内容 |
|---|---|
| `~/.cache/bake/<triple>/` | 该目标的工具链产物：C 运行时对象、`std` 模块、libc++ 对象、sanitizer runtime——全部内容寻址 |
| `~/.cache/bake/src/` | 已获取的远程依赖源码（git checkout、解包归档） |

首次 `bake build -t <triple>` 付出一次运行时构建成本；之后的构建——任何项目里——都复用缓存产物。带版本后缀的目标（`x86_64-linux-gnu.2.36`）有独立的缓存条目，与不带后缀的 triple 分开。


工具链产物是有界的：每次写入后，目标命名空间会按最近最少使用裁剪到配额以内（默认 8 GiB，可用环境变量
`BAKE_CACHE_LIMIT_MB` 覆盖）。淘汰只删除内容寻址的产物目录——被删除的产物在下一次未命中时直接重建；
`src/` 下的源码缓存永不裁剪。

## 指纹与增量性

- 每个构建动作均按内容生成指纹：输入、标志、define、目标、profile。
- 对一个源文件的修改会重新编译该 TU 及其依赖方（包括模块依赖边），不会影响其他内容。
- profile 和目标的变更会选择不同的输出目录与独立项目状态上下文。
- Core 上下文身份还包含规范化 workspace、显式选择的 member（或全部 member）、
  排序后的 feature，以及当前 bake 编译器的规范路径、版本与内容摘要。
- 项目快照记录每个相关 workspace/member `bake.toml`、workspace 实际 `bake.lock`
  路径（包括文件不存在的状态）以及所有参与的 `build.cpp` 的规范路径、存在状态与
  精确文件系统修改时间。只有完整记录及目标/profile/features/compiler 上下文完全
  相同时，声明状态才会复用。
- 时间戳变新、变旧、仅 touch、创建或删除都会使声明记录失效。若修改内容后恢复
  完全相同的 mtime，则该 mtime 记录不会失效；无关文件/目录、环境变量与
  `bake.build` wrapper 的变化也不会使它失效。
- 这条 mtime 规则只适用于声明复用。普通项目源码和头文件会被读取并以 SHA-256
  校验构建输入身份和 ready 输出复用；即使恢复相同的大小与 mtime，源码编辑也会被发现。
- `build.cpp` 决定输入声明；包身份、语言/profile 选项、feature、目标设置和链接配置
  始终以 `bake.toml` 为准。

## 写入边界

- 项目构建产物与 Core 状态均位于 `out/`；已获取源码与工具链产物位于
  `~/.cache/bake/`。
- `build` 与 `compdb` 在 workspace 根写入 `compile_commands.json`（或
  `compdb` 显式指定的输出路径）。`add`/`update` 编辑 `bake.toml` 与
  `bake.lock`。独立 `cc`/`c++`/`ar` 只写入其参数指定的路径。
- 在 `build` 期间绝不移动已锁定依赖的 tag。
- 复用 Core 与分析记录之前会先验证；无效的分析条目会被忽略或安全替换。仅当你
  明确要丢弃完整的项目输出与状态树时，才使用 `bake clean`。
