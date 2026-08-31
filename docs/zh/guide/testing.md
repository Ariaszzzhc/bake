# 测试

bake 的测试机制以注册表为基础：一个包注册具名测试，每项测试均由一个 binary 支持；`bake test` 会构建并运行它们。

## 注册测试

测试在 `build.cpp` 中声明（第 2 或第 3 档——参见[构建脚本](build-scripts.md)）。每个注册项会命名一个测试及其实现该测试的 binary：

```cpp
// build.cpp
import bake;

int main() {
    bake::Builder b;

    // Binaries: one executable per test suite
    b.binary("unit_parser").sources("tests/unit_parser.cpp");
    b.binary("unit_string").sources("tests/unit_string.cpp");
    b.binary("integration").sources("tests/integration.cpp");

    // Registrations: which binaries are tests
    b.add_test("parser-suite", "unit_parser").set_default();
    b.add_test("string-suite", "unit_string");

    return b.build();
}
```

`.set_default()` 标记在未指定名称时由 `bake test` 运行的测试。最多只能有一个默认测试——之后调用 `set_default()` 会清除先前的默认设置。

测试 binary 是常规 binary：它们链接包的库，可以 `import` 其公开模块，也可以使用其公开头文件。应将它们写成具有自身 `main()` 的普通可执行文件；退出码为 0 表示通过，其他任何值表示失败。

## 运行

```bash
bake test                # the default test (error if none registered)
bake test parser-suite   # one named test
bake test --all          # every registered test, in registration order
bake test -p core        # tests of one workspace member
```

`bake test` 会先构建（使用与 `bake build` 相同的引擎和增量缓存），然后运行选定的测试 binary，并将其 stdout/stderr 传递到终端。只要任一选定测试失败，它便会以非零状态退出。

## 约定

没有内置的测试框架。常见安排包括：

- 每个测试套件一个 binary（parser、string、integration），各自输出其进度
- 一个共享的 `tests/test_main.hpp`，包含由每个套件引入的小型断言宏
- 将大型套件进一步拆分，每个套件使用 `add_test`，并在 CI 中使用 `--all`

对于工作区，每个成员保留自身的注册项；`bake test -p <member>` 会限定运行范围。

## 示例

一个最小的断言辅助工具：

```cpp
// tests/test_main.hpp
#pragma once
#include <cstdio>
#include <cstdlib>

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                               \
        }                                                               \
    } while (0)
```

```cpp
// tests/unit_parser.cpp
#include "test_main.hpp"
#include <mylib/parser.hpp>

int main() {
    CHECK(parse("1 + 2") == 3);
    std::puts("parser OK");
    return 0;
}
```