import modlib.api;
import modlib.detail;
#include "modlib_marker.hpp"

int main() {
    return modlib_api_value() == 42 &&
           modlib_combined_value() == 52 &&
           modlib_marker_value() == 100
        ? 0 : 1;
}
