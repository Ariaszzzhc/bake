#include "archive.hpp"
#include "base.hpp"
#include "shared.hpp"

int main() {
    return base_value() == 7 &&
                   archive_value() == 12 &&
                   shared_value() == 30
        ? 0
        : 1;
}
