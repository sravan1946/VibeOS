/* threads.c - four threads sum a range under a mutex: tcc threads.c -o threads && ./threads */
#include <stdio.h>
#include <pthread.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static long long total;

static void *work(void *arg) {
    long id = (long)arg;
    long long part = 0;
    for (long i = id * 250000; i < (id + 1) * 250000; i++) part += i;
    pthread_mutex_lock(&lock);
    total += part;
    pthread_mutex_unlock(&lock);
    printf("thread %ld (tid %d) done\n", id, pthread_self());
    return (void *)id;
}

int main(void) {
    pthread_t t[4];
    for (long i = 0; i < 4; i++) pthread_create(&t[i], NULL, work, (void *)i);
    for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);
    printf("sum 0..999999 = %lld (expected %lld)\n", total, 999999LL * 1000000 / 2);
    return 0;
}
