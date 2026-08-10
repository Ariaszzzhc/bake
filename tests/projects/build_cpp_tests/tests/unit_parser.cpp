import std;

#include <build_cpp_tests/api.hpp>

int main() {
    if (library_value() != 42) return 1;
    std::println("BAKE_TEST_PARSER_RAN");
    return 0;
}
