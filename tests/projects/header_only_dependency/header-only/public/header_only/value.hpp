#pragma once

#include <cmath>

#ifndef BAKE_HEADER_ONLY_FAST_PATH
#error "header-only option define was not forwarded to consumers"
#endif

#ifndef BAKE_HEADER_ONLY_VERSION
#error "header-only package defines were not forwarded to consumers"
#endif

inline int header_only_value(double input) {
    static_assert(BAKE_HEADER_ONLY_VERSION_MAJOR == 2);
    static_assert(BAKE_HEADER_ONLY_VERSION_MINOR == 4);
    static_assert(BAKE_HEADER_ONLY_VERSION_PATCH == 6);
    return BAKE_HEADER_ONLY_FAST_PATH && std::sqrt(input) == 2.0 ? 42 : 0;
}
