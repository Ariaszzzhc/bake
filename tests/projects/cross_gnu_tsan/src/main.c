#include <pthread.h>
int v;
static void *worker(void *arg) {
    (void)arg;
    for (int i = 0; i < 100000; ++i) v++;
    return 0;
}
int main(void) {
    pthread_t a, b;
    pthread_create(&a, 0, worker, 0);
    pthread_create(&b, 0, worker, 0);
    pthread_join(a, 0);
    pthread_join(b, 0);
    return 0;
}
