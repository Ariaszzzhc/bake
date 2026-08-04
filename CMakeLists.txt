cmake_minimum_required(VERSION 3.28)

project(bake
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- Third-party header-only libraries ----

add_library(tomlplusplus INTERFACE)
target_include_directories(tomlplusplus INTERFACE
    ${CMAKE_SOURCE_DIR}/third_party/tomlplusplus)

add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE
    ${CMAKE_SOURCE_DIR}/third_party/nlohmann/public)

# ---- libbake (shared library) ----

add_library(libbake SHARED
    libbake/src/cabi/api.cpp
)

target_sources(libbake
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
            libbake/src/bake.util.cppm
            libbake/src/bake.project.cppm
            libbake/src/bake.compiler.cppm
            libbake/src/bake.engine.cppm
            libbake/src/bake.package.cppm
            libbake/src/bake.cli.cppm
)

set_target_properties(libbake PROPERTIES OUTPUT_NAME "bake")

target_link_libraries(libbake PRIVATE tomlplusplus nlohmann_json)
target_include_directories(libbake PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/tomlplusplus/public
    ${CMAKE_SOURCE_DIR}/third_party/nlohmann/public
    ${CMAKE_SOURCE_DIR}/libbake/src/cabi
)
target_compile_definitions(libbake PRIVATE
    BAKE_VERSION="${PROJECT_VERSION}"
    BAKE_SRC_DIR="${CMAKE_SOURCE_DIR}"
    BAKE_LIB_DIR="${CMAKE_BINARY_DIR}"
)
target_compile_features(libbake PRIVATE cxx_std_23)

# ---- bake (executable) ----

add_executable(bake bake/src/main.cpp)
target_link_libraries(bake PRIVATE libbake)
