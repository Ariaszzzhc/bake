#include <stdio.h>

_Static_assert(__STDC_VERSION__ >= 201710L, "C17 or newer is required");

int main(void) {
    int values[3] = {[2] = 42};
    puts("Hello from C!");
    return values[2] == 42 ? 0 : 1;
}
