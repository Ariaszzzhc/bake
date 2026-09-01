#include <stdlib.h>
int main(void) {
    char *p = (char *)malloc(8);
    p[8] = 1;  // heap-buffer-overflow, one past the 8-byte region
    free(p);
    return 0;
}
