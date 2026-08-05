#include <stdio.h>

_Static_assert(__STDC_VERSION__ >= 201710L, "C17 or newer is required");

int left_value(void);
int right_value(void);

int main(void) {
    int values[3] = {[2] = 42};
    puts("Hello from C!");
    return values[2] == 42 && left_value() + right_value() == 3 ? 0 : 1;
}
