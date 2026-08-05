cmake_minimum_required(VERSION 3.15)
project(cmathlib C)

set(CMAKE_C_STANDARD 11)

add_library(cmathlib STATIC src/cmathlib.c)
target_include_directories(cmathlib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

install(TARGETS cmathlib
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)
install(DIRECTORY include/ DESTINATION include)
