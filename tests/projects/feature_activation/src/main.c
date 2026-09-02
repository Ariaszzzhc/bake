#include <stdio.h>

int math_value(void);

int main(void) {
    int v = math_value();
    printf("%d\n", v);
    return (v == 8 || v == 1) ? 0 : 3;
}
