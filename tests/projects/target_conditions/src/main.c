#include <family.h>
#include <exact.h>

#if TARGET_FAMILY != 1
#error "family target table did not match"
#endif

#if TARGET_EXACT != 1
#error "exact target table did not match"
#endif

#if TARGET_PRIORITY != 2
#error "more-specific target flags must be merged last"
#endif

#ifdef WRONG_ARCH_MATCH
#error "non-matching target table leaked into this target"
#endif

int main(void) {
    return FAMILY_HEADER == 1 && EXACT_HEADER == 1 ? 0 : 1;
}
