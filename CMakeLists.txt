cmake_minimum_required(VERSION 3.30)

# ---- Enable import std (experimental gate) ----
# Must be set before project() so the CXX toolchain probe detects support.
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD
    "f35a9ac6-8463-4d38-8eec-5d6008153e7d")

# ---- Locate libc++.modules.json (system Clang layout) ----
# CMake needs this to discover std.cppm for import std.
if(DEFINED CMAKE_CXX_COMPILER AND EXISTS "${CMAKE_CXX_COMPILER}")
    get_filename_component(_cxx_bindir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(_cxx_prefix "${_cxx_bindir}" DIRECTORY)
    if(EXISTS "${_cxx_prefix}/lib/c++/libc++.modules.json")
        set(CMAKE_CXX_STDLIB_MODULES_JSON "${_cxx_prefix}/lib/c++/libc++.modules.json")
    endif()
endif()

project(bake
    VERSION 0.1.0
    LANGUAGES C CXX
)

# ── LLVM/Clang/LLD ──
# Default: use self-built LLVM from submodule (scripts/build-llvm.sh).
# Override: -DLLVM_DIR=/path/to/lib/cmake/llvm  (e.g. Homebrew for dev)
if(NOT DEFINED LLVM_DIR)
    set(LLVM_DIR "${CMAKE_SOURCE_DIR}/external/llvm-install/lib/cmake/llvm" CACHE PATH "LLVM CMake config dir")
endif()
if(NOT DEFINED Clang_DIR)
    set(Clang_DIR "${CMAKE_SOURCE_DIR}/external/llvm-install/lib/cmake/clang" CACHE PATH "Clang CMake config dir")
endif()
if(NOT DEFINED LLD_DIR)
    set(LLD_DIR "${CMAKE_SOURCE_DIR}/external/llvm-install/lib/cmake/lld" CACHE PATH "LLD CMake config dir")
endif()

find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)
find_package(LLD REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} at ${LLVM_DIR}")

# Derive LLVM installation prefix from LLVM_DIR (always <prefix>/lib/cmake/llvm).
# More robust than calling llvm-config, which may not be built (LLVM_BUILD_TOOLS=OFF).
get_filename_component(LLVM_PREFIX "${LLVM_DIR}/../../.." ABSOLUTE)
message(STATUS "LLVM prefix: ${LLVM_PREFIX}")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_MODULE_STD ON)

# Ensure libc++ (required for import std with Clang)
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_compile_options(-stdlib=libc++ -Wno-reserved-module-identifier)
    add_link_options(-stdlib=libc++)
endif()

# ---- bake (single executable: CLI + compiler + engine + package) ----

add_executable(bake
    bake/src/main.cpp
)

target_sources(bake
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
            bake/src/json.cppm
            bake/src/toml.cppm
            bake/src/util.cppm
            bake/src/buildsystem/project.cppm
            bake/src/buildsystem/moid.cppm
            bake/src/buildsystem/graph.cppm
            bake/src/toolchain/lld.cppm
            bake/src/toolchain/target.cppm
            bake/src/buildsystem/cmdgen.cppm
            bake/src/toolchain/runtime.cppm
            bake/src/toolchain/cc1.cppm
            bake/src/toolchain/cc1as.cppm
            bake/src/toolchain/driver.cppm
            bake/src/buildsystem/engine.cppm
            bake/src/buildsystem/package.cppm
)

# stage0 implementation units of bake.json / bake.toml — hand-written,
# zero third-party sources. The self-hosted build compiles the
# nlohmann/toml++-backed units (src/json.cpp, src/toml.cpp) instead,
# with the packages coming from bake-pkgs.
target_sources(bake PRIVATE
    bake/src/stage0/json.cpp
    bake/src/stage0/toml.cpp
)

target_compile_features(bake PRIVATE cxx_std_23)
set_target_properties(bake PROPERTIES CXX_MODULE_STD ON ENABLE_EXPORTS ON)

# Static linking against all LLVM/Clang/LLD libraries.
file(GLOB BAKE_LLVM_STATIC_LIBS  "${LLVM_PREFIX}/lib/libLLVM*.a")
file(GLOB BAKE_CLANG_STATIC_LIBS "${LLVM_PREFIX}/lib/libclang*.a")
file(GLOB BAKE_LLD_STATIC_LIBS   "${LLVM_PREFIX}/lib/liblld*.a")

# The LLVM archives have cyclic interdependencies and the alphabetical glob
# order satisfies neither GNU ld nor lld's single-pass archive scan
# (libclangAST.a precedes libclangCodeGen.a, whose AST refs then dangle).
# Group them so resolution order doesn't matter; ld64 iterates to a closure
# on its own and rejects the flag.
if(APPLE)
    target_link_libraries(bake PRIVATE
        ${BAKE_CLANG_STATIC_LIBS}
        ${BAKE_LLD_STATIC_LIBS}
        ${BAKE_LLVM_STATIC_LIBS}
    )
else()
    target_link_libraries(bake PRIVATE
        -Wl,--start-group
        ${BAKE_CLANG_STATIC_LIBS}
        ${BAKE_LLD_STATIC_LIBS}
        ${BAKE_LLVM_STATIC_LIBS}
        -Wl,--end-group
    )
endif()

# Vendored static compression libraries (built from git submodules).
# No system dependency — fully reproducible.
find_library(ZLIB_STATIC_LIB NAMES libz.a
  PATHS ${CMAKE_SOURCE_DIR}/external/zlib-install/lib
  NO_DEFAULT_PATH REQUIRED)
find_library(ZSTD_STATIC_LIB NAMES libzstd.a
  PATHS ${CMAKE_SOURCE_DIR}/external/zstd-install/lib
  NO_DEFAULT_PATH REQUIRED)
target_link_libraries(bake PRIVATE ${ZLIB_STATIC_LIB} ${ZSTD_STATIC_LIB})

# dead_strip removes unreferenced code from the final binary.
if(APPLE)
    target_link_options(bake PRIVATE -Wl,-dead_strip)
endif()

target_include_directories(bake PRIVATE
    ${LLVM_INCLUDE_DIRS}
    ${CLANG_INCLUDE_DIRS}
    ${LLD_INCLUDE_DIRS}
)
target_compile_definitions(bake PRIVATE
    BAKE_VERSION="${PROJECT_VERSION}"
)

# LLVM is built with LLVM_ENABLE_RTTI=ON, so no -fno-rtti needed anywhere.

# Runtime library search path — only self-built LLVM lib dir needed
# (bake is now a single statically-linked executable, no libbake.dylib).
if(APPLE)
    set_target_properties(bake PROPERTIES
        BUILD_RPATH "${LLVM_PREFIX}/lib"
        INSTALL_RPATH "@loader_path/../lib"
    )
elseif(UNIX)
    set_target_properties(bake PROPERTIES
        BUILD_RPATH "${LLVM_PREFIX}/lib"
        INSTALL_RPATH "$ORIGIN/../lib"
    )
endif()

# ---- Installation ----

include(GNUInstallDirs)
install(TARGETS bake
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES
    lib/bake/bake.build.cppm
    DESTINATION lib/bake
)

# ---- Tests ----

enable_testing()

# C++ test runner — spawns the bake binary on real project fixtures.
add_executable(test_runner tests/test_runner.cpp)
target_compile_features(test_runner PRIVATE cxx_std_17)
target_compile_definitions(test_runner PRIVATE
    BAKE_SRC_DIR="${CMAKE_SOURCE_DIR}"
)

add_executable(graph_test tests/graph_test.cpp)
target_compile_features(graph_test PRIVATE cxx_std_23)
set_target_properties(graph_test PROPERTIES CXX_MODULE_STD ON)

# graph_test imports bake.graph + bake.moid (and their transitive deps).
# Provide these as module sources directly — they are pure data structures
# with no LLVM linkage, so no static libs needed.
target_sources(graph_test
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
            bake/src/json.cppm
            bake/src/toml.cppm
            bake/src/util.cppm
            bake/src/toolchain/target.cppm
            bake/src/buildsystem/project.cppm
            bake/src/buildsystem/moid.cppm
            bake/src/buildsystem/graph.cppm
)

target_sources(graph_test PRIVATE
    bake/src/stage0/json.cpp
    bake/src/stage0/toml.cpp
)

add_executable(parser_test tests/parser_test.cpp)
target_compile_features(parser_test PRIVATE cxx_std_23)
set_target_properties(parser_test PROPERTIES CXX_MODULE_STD ON)
target_sources(parser_test
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
            bake/src/json.cppm
            bake/src/toml.cppm
)
target_sources(parser_test PRIVATE
    bake/src/stage0/json.cpp
    bake/src/stage0/toml.cpp
)

add_test(NAME parser_test COMMAND parser_test)

add_test(
    NAME bake_test_suite
    COMMAND test_runner $<TARGET_FILE:bake>
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(bake_test_suite PROPERTIES
    ENVIRONMENT "CXX=${CMAKE_CXX_COMPILER}"
)

add_test(NAME graph_test COMMAND graph_test)
