#include <header_only/value.hpp>

int main() {
    volatile double input = 4.0;
    return header_only_value(input) == 42 ? 0 : 1;
}
