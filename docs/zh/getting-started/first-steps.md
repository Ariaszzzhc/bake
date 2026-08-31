# 第一步

## 创建项目

```bash
bake init hello
cd hello
```

这会生成以下脚手架：

```
hello/
├── bake.toml
├── src/
│   └── main.cpp
├── public/
└── tests/
```

实用标志：

- `--type <executable|lib|dylib>`——moid 类型（默认为 `executable`）
- `--std <c11|c17|c23|c++17|c++20|c++23>`——语言标准（默认为 `c++20`）

C 标准（例如 `--std c17`）会生成真正的 C 源文件。`lib` 项目会获得 `src/lib.cpp`，并在 `public/<name>/` 下获得一个公开头文件。

## 构建并运行

```bash
bake build     # compile + link, in parallel, incremental
bake run       # build if needed, then run the executable
```

```
   Building hello v0.1.0
    Finished in 0.52s
Hello from hello!
```

`bake run` 会将尾随的位置参数转发给程序：

```bash
bake run some-file.txt
```

以 `-` 开头的参数会被解析为 bake 自身的选项——目前尚不支持
`--` 分隔符，因此请通过包装脚本或环境向程序传递带连字符的标志。

如果工作区包含多个 executable，bake 会要求使用 `-p <member>` 选择一个。

## 默认项目布局

除 `bake.toml` 外无需任何配置，bake 会按约定发现输入：

| 目录 | 发现为 |
|---|---|
| `src/**/*.{cpp,cc,cxx,c}` | 私有源文件 |
| `src/**/*.cppm` | 私有模块接口 |
| `public/**/*.cppm` | **公开**模块接口，导出给使用者 |
| `public/` | 公开的包含目录（头文件由你自行放置） |

在 `public/` 下找到的模块接口会被编译并导出给依赖包；`src/` 下的模块接口则保持内部可见。头文件不会被编译——`public/` 仅会作为使用者的包含路径加入。

可通过 `bake.toml` 中的 `[sources]` 配置扩展名集合（参见[清单参考](../reference/manifest.md)）。

## 增量构建

再次运行 `bake build` 的开销很小：bake 会为每项操作计算指纹（基于内容），且只重新执行发生变化的内容。编辑一个源文件会重新编译该文件并重新链接。切换 profile（`--release`）或目标（`-t`）会构建至独立的输出目录，且两棵目录树都仍然有效。

## 输出位置

```
out/
└── aarch64-apple-darwin/    # one directory per target triple
    ├── bin/                 # executables
    ├── lib/                 # .a / .dylib
    ├── .obj/                # object files
    ├── .bmi/                # module PCMs
    └── .bake/               # resolved declarations, fingerprints
```

`bake clean` 会移除 `out/`。完整细节参见[输出与缓存布局](../reference/output-and-cache.md)。

## 模块初览

在 `bake.toml` 中将标准设为 C++20 或更高版本：

```toml
[language]
cxx = "c++23"
```

随后即可直接编写模块——无需标志、无需 `clang-scan-deps`，也无需模块映射文件：

```cpp
// src/math.cppm
export module math;
export int triple(int x) { return x * 3; }
```

```cpp
// src/main.cpp
import std;
import math;

int main() {
    std::println("{}", triple(14));    // 42
}
```

`import std;` 开箱即用：bake 会从其内置的 libc++ 预构建 `std.pcm`，并将其缓存。

## 下一步

- [依赖](../guide/dependencies.md)
- [构建脚本](../guide/build-scripts.md)——当默认发现不足时
- [测试](../guide/testing.md)
