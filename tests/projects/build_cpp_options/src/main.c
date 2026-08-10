#include <stdio.h>
#include <string.h>

int left_value(void);
int right_value(void);
const char *selected_backend(void);

#ifndef BAKE_OPTION_APP_DIAGNOSTICS
#error "false options must still be defined as 0"
#endif

#ifndef BAKE_OPTION_APP_NATIVE_BACKEND
#error "hyphenated option did not produce a normalized macro"
#endif

#ifndef BAKE_OPTION_APP_NAME
#error "package name macro is missing"
#endif

#ifndef BAKE_OPTION_APP_VERSION
#error "package version macro is missing"
#endif

int main(void) {
    if (left_value() + right_value() != 3) return 2;
    if (strcmp(BAKE_OPTION_APP_NAME, "option-app") != 0) return 3;
    if (strcmp(BAKE_OPTION_APP_VERSION, "0.1.0") != 0) return 4;
    if (BAKE_OPTION_APP_VERSION_MAJOR != 0 ||
        BAKE_OPTION_APP_VERSION_MINOR != 1 ||
        BAKE_OPTION_APP_VERSION_PATCH != 0) return 5;
    printf("%s|%d\n", selected_backend(), BAKE_OPTION_APP_DIAGNOSTICS);
    return 0;
}
