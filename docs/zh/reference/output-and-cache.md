# 输出与缓存布局

## 项目输出：`out/`

所有构建产物均位于项目根目录下，并按每个目标三元组划分为一个子树：

```
out/
├── aarch64-apple-darwin/      # 示例中的本机目标
│   ├── bin/                   # 可执行文件（主目标 + build.cpp 声明的 binaries）
│   ├── lib/                   # lib → .a，dylib → .dylib/.so/.dll
│   ├── .obj/                  # 对象文件
│   ├── .bmi/                  # 模块 PCM 文件
│   └── .bake/                 # 声明、指纹
└── .bake/                     # 宿主级产物（编译后的 build.cpp 脚本）
```

- `bin/` 存放 moid 的可执行文件，以及每个 `b.binary()` 目标。
- `lib/` 存放 lib 的 `lib<name>.a` 以及 dylib 的共享库。依赖方会链接该归档，并接收其公开头文件和模块。
- 目标三元组目录内的 `.bake/` 存放每个 moid 已解析的构建声明、内容指纹和动作图；这正是增量重建得以实现的基础。删除它会强制重新解析（但不会重新编译其对象文件仍保持有效指纹的源文件）。

`bake clean` 会完全删除 `out/`。

## 全局缓存：`~/.cache/bake/`

在各项目之间共享，按目标与内容为键：

| 路径 | 内容 |
|---|---|
| `~/.cache/bake/<triple>/` | 该目标的工具链产物：C 运行时对象、`std` 模块、libc++ 对象、sanitizer runtime——全部内容寻址 |
| `~/.cache/bake/src/` | 已获取的远程依赖源码（git checkout、解包归档） |

首次 `bake build -t <triple>` 付出一次运行时构建成本；之后的构建——任何项目里——都复用缓存产物。带版本后缀的目标（`x86_64-linux-gnu.2.36`）有独立的缓存条目，与不带后缀的 triple 分开。

## 指纹与增量性

- 每个构建动作均按内容生成指纹：输入、标志、define、目标、profile。
- 对一个源文件的修改会重新编译该 TU 及其依赖方（包括模块依赖边），不会影响其他内容。
- profile 和目标的变更会产生不同的指纹和输出目录；来回切换不会造成缓存抖动。
- 仅当该构建脚本、`bake.toml` 或 `bake.build` 模块比缓存的声明更新时，才会重新运行 `build.cpp` 脚本。

## bake 永不执行的操作

- 除 `out/` 和 `~/.cache/bake/` 外，绝不写入其他位置（`add`/`update` 对 `bake.lock`/`bake.toml` 的编辑除外）。
- 在 `build` 期间绝不移动已锁定依赖的 tag。
- 绝不要求干净构建：缓存损坏是执行 `rm -rf out/` 的唯一理由。
