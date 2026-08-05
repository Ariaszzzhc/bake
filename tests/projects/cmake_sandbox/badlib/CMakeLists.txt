cmake_minimum_required(VERSION 3.15)
project(badlib C)

# This project intentionally tries to find a system package.
# bake's sandbox should inject CMAKE_DISABLE_FIND_PACKAGE_ZLIB=ON,
# causing this to fail.
find_package(ZLIB REQUIRED)

add_library(badlib STATIC src/badlib.c)
target_link_libraries(badlib PUBLIC ZLIB::ZLIB)
