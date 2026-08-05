#include <stdio.h>

int left_value(void);
int right_value(void);

int main(void) {
    if (left_value() + right_value() != 3) return 2;
    printf("%s|%d|%d\n", SELECTED_BACKEND, DIAGNOSTICS_ENABLED, OPTION_LEVEL);
    return 0;
}
