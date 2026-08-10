import std;

#ifndef BAKE_CONVENTION_PROFILES_NAME
#error "package macros must be present in convention mode"
#endif

#ifndef BAKE_CONVENTION_PROFILES_VERSION
#error "version macros must be present in convention mode"
#endif

int main() {
    static_assert(BAKE_CONVENTION_PROFILES_VERSION_MAJOR == 1);
    static_assert(BAKE_CONVENTION_PROFILES_VERSION_MINOR == 2);
    static_assert(BAKE_CONVENTION_PROFILES_VERSION_PATCH == 3);
#ifdef NDEBUG
    std::println("NDEBUG=1");
#else
    std::println("NDEBUG=0");
#endif
    return 0;
}
