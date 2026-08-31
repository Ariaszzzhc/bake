# 输出与缓存布局

## 项目输出：`out/`

所有构建产物均位于项目根目录下，并按每个目标三元组划分为一个子树：

```
out/
├── aarch64-apple-darwin/      # native in this example
│   ├── bin/                   # executables (main + build.cpp binaries)
│   ├── lib/                   # lib → .a, dylib → .dylib/.so/.dll
│   ├── .obj/                  # object files
│   ├── .bmi/                  # module PCM files
│   └── .bake/                 # declarations, fingerprints, graph.json
└── .bake/                     # host-level artifacts (compiled build.cpp scripts)
```

- `bin/` 存放 moid 的可执行文件，以及每个 `b.binary()` 目标。
- `lib/` 存放 lib 的 `lib<name>.a` 以及 dylib 的共享库。依赖方会链接该归档，并接收其公开头文件和模块。
- 目标三元组目录内的 `.bake/` 存放每个 moid 已解析的 `MoidDeclaration` JSON、内容指纹和动作图；这正是增量重建得以实现的基础。删除它会强制重新解析（但不会重新编译其对象文件仍保持有效指纹的源文件）。

`bake clean` 会完全删除 `out/`。

## 全局缓存：`~/.cache/bake/`

在各项目之间共享，并以工具链和内容为键：

| 路径 | 内容 |
|---|---|
| `~/.cache/bake/<key>/std/` | 供 `import std;` 使用的、按（目标、std 版本）区分的预构建 `std.pcm` |
| `~/.cache/bake/libcxx-objects/` | 按目标区分的静态 libc++ / libc++abi / libunwind 对象文件 |
| （包缓存） | 已获取的远程依赖（git checkout） |

跨目标 libc 构建（musl CRT、MinGW-w64 CRT + winpthreads）也会按目标并依照相同方案缓存，因此首次执行 `bake build -t x86_64-linux-musl` 时只需付出一次构建开销。

## 指纹与增量性

- 每个构建动作均按内容生成指纹：输入、标志、define、目标、profile。
- 对一个源文件的修改会重新编译该 TU 及其依赖方（包括模块依赖边），不会影响其他内容。
- profile 和目标的变更会产生不同的指纹和输出目录；来回切换不会造成缓存抖动。
- 仅当该构建脚本、`bake.toml` 或 `bake.build` 模块比缓存的声明更新时，才会重新运行 `build.cpp` 脚本。

## bake 永不执行的操作

- 除 `out/` 和 `~/.cache/bake/` 外，绝不写入其他位置（`add`/`update` 对 `bake.lock`/`bake.toml` 的编辑除外）。
- 在 `build` 期间绝不移动已锁定依赖的 tag。
- 绝不要求干净构建：缓存损坏是执行 `rm -rf out/` 的唯一理由。
