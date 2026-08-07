export module modlib.detail;

import modlib.api;

export int modlib_combined_value() {
    return modlib_api_value() + 10;
}
