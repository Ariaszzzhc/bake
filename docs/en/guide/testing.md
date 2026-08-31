# Testing

bake's test story is registry-based: a package registers named tests, each backed by a binary; `bake test` builds and runs them.

## Registering tests

Tests are declared in `build.cpp` (tier 2 or 3 — see [Build Scripts](build-scripts.md)). Each registration names a test and the binary that implements it:

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

`.set_default()` marks the test `bake test` runs when no name is given. At most one default exists — a later `set_default()` clears the earlier one.

Test binaries are regular binaries: they link the package's library, can `import` its public modules, and can use its public headers. Write them as ordinary executables with their own `main()`; exit code 0 is pass, anything else is fail.

## Running

```bash
bake test                # the default test (error if none registered)
bake test parser-suite   # one named test
bake test --all          # every registered test, in registration order
bake test -p core        # tests of one workspace member
```

`bake test` builds first (same engine, same incremental caching as `bake build`), then runs the selected test binaries with their stdout/stderr passed through to your terminal. It exits non-zero if any selected test fails.

## Conventions

There is no built-in test framework. Common arrangements:

- one binary per suite (parser, string, integration), each printing its own progress
- a shared `tests/test_main.hpp` with tiny assertion macros, included by each suite
- heavy suites split further, with `add_test` per suite and `--all` in CI

For a workspace, each member keeps its own registrations; `bake test -p <member>` scopes the run.

## Example

A minimal assertion helper:

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
