#include "archive.hpp"
#include "shared.hpp"

int shared_value() {
    return archive_value() + 18;
}
