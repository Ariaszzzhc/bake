import std;

#ifndef BAKE_PROFILE_CONFIGURATION_NAME
#error "package name macro must be present after manifest configuration"
#endif

#ifndef BAKE_PROFILE_CONFIGURATION_VERSION
#error "package version macros must be present after manifest configuration"
#endif

int main() {
    static_assert(BAKE_PROFILE_CONFIGURATION_VERSION_MAJOR == 1);
    static_assert(BAKE_PROFILE_CONFIGURATION_VERSION_MINOR == 2);
    static_assert(BAKE_PROFILE_CONFIGURATION_VERSION_PATCH == 3);
#ifdef NDEBUG
    std::println("NDEBUG=1");
#else
    std::println("NDEBUG=0");
#endif
    return 0;
}
