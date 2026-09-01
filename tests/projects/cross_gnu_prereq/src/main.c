// gettid() was added to glibc 2.30; its declaration in
// <bits/unistd_ext.h> is gated on __USE_GNU (i.e. _GNU_SOURCE) and on
// __GLIBC_PREREQ (2, 30). bake vendors headers from the newest glibc
// and pins __GLIBC_MINOR__ to the target version, so this file compiles
// only for targets >= 2.30 — the default 2.28 target must reject it at
// compile time.
#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>

#if !__GLIBC_PREREQ(2, 30)
#error "target must present the glibc >= 2.30 header surface"
#endif

int main(void) {
    printf("tid %ld\n", (long)gettid());
    return 0;
}
