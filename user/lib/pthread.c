/* pthread.c - pthreads-lite over SYS_thread_create/exit/join and SYS_futex.
 * Linking this file also turns on __libc_lock (weak in malloc.c and stdio.c). */
#include <pthread.h>
#include <stdlib.h>
#include <errno.h>
#include <syscall.h>

#define P(x) ((int)(uintptr_t)(x))

int futex_wait(volatile int *addr, int val) { return __sysret(__syscall3(SYS_futex, P(addr), FUTEX_WAIT, val)); }
int futex_wake(volatile int *addr) { return __sysret(__syscall3(SYS_futex, P(addr), FUTEX_WAKE, 0)); }

/* ---- mutexes (Drepper, "Futexes Are Tricky", mutex 2) ---- */

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr) { m->v = 0; return 0; }
int pthread_mutex_destroy(pthread_mutex_t *m) { return m->v ? EBUSY : 0; }

int pthread_mutex_lock(pthread_mutex_t *m) {
    int c = __sync_val_compare_and_swap(&m->v, 0, 1);
    if (!c) return 0;
    if (c != 2) c = __sync_lock_test_and_set(&m->v, 2);
    while (c) {
        futex_wait(&m->v, 2);
        c = __sync_lock_test_and_set(&m->v, 2);
    }
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *m) { return __sync_val_compare_and_swap(&m->v, 0, 1) ? EBUSY : 0; }

int pthread_mutex_unlock(pthread_mutex_t *m) {
    if (__sync_fetch_and_sub(&m->v, 1) != 1) {          /* there were waiters */
        m->v = 0;
        futex_wake(&m->v);
    }
    return 0;
}

/* ---- condition variables: a sequence number the waiters sleep on ---- */

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *attr) { c->seq = 0; return 0; }
int pthread_cond_destroy(pthread_cond_t *c) { return 0; }

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    int s = c->seq;
    pthread_mutex_unlock(m);
    futex_wait(&c->seq, s);                              /* returns at once if a signal came in between */
    pthread_mutex_lock(m);
    return 0;
}

int pthread_cond_signal(pthread_cond_t *c) { __sync_fetch_and_add(&c->seq, 1); futex_wake(&c->seq); return 0; }
int pthread_cond_broadcast(pthread_cond_t *c) { return pthread_cond_signal(c); }   /* the kernel wakes all anyway */

static pthread_mutex_t once_lock = PTHREAD_MUTEX_INITIALIZER;
int pthread_once(pthread_once_t *o, void (*fn)(void)) {
    if (o->state == 2) return 0;
    pthread_mutex_lock(&once_lock);                      /* ponytail: one lock for every once; fn mustn't call pthread_once */
    if (!o->state) { o->state = 1; fn(); o->state = 2; }
    pthread_mutex_unlock(&once_lock);
    return 0;
}

/* ---- the libc lock: recursive, only taken once a second thread exists ---- */

static int threaded;
static pthread_mutex_t libc_mx = PTHREAD_MUTEX_INITIALIZER;
static volatile int libc_owner, libc_depth;

void __libc_lock(void) {
    if (!threaded) return;
    int me = __syscall0(SYS_gettid);
    if (libc_owner == me) { libc_depth++; return; }
    pthread_mutex_lock(&libc_mx);
    libc_owner = me;
    libc_depth = 1;
}

void __libc_unlock(void) {
    if (!threaded || --libc_depth) return;
    libc_owner = 0;
    pthread_mutex_unlock(&libc_mx);
}

/* ---- threads ---- */

struct thr { pthread_t tid; void *(*fn)(void *); void *arg; void *stack; struct thr *next; };
static struct thr *thrs;                                 /* under the libc lock */

static void thread_start(struct thr *t) { pthread_exit(t->fn(t->arg)); }

int pthread_attr_init(pthread_attr_t *a) { a->stacksize = 128 * 1024; return 0; }
int pthread_attr_destroy(pthread_attr_t *a) { return 0; }
int pthread_attr_setstacksize(pthread_attr_t *a, size_t n) {
    if (n < PTHREAD_STACK_MIN) return EINVAL;
    a->stacksize = n;
    return 0;
}
int pthread_attr_getstacksize(const pthread_attr_t *a, size_t *n) { *n = a->stacksize; return 0; }

int pthread_create(pthread_t *tid, const pthread_attr_t *attr, void *(*fn)(void *), void *arg) {
    size_t size = attr ? attr->stacksize : 128 * 1024;
    struct thr *t = malloc(sizeof *t);
    void *stack = t ? malloc(size) : NULL;
    if (!stack) { free(t); return EAGAIN; }
    t->fn = fn; t->arg = arg; t->stack = stack;
    /* thread_start(t) as if called: (esp + 4) 16-aligned, return address 0 */
    uint32_t *sp = (uint32_t *)(((uintptr_t)stack + size) & ~15u) - 5;
    sp[0] = 0;
    sp[1] = (uint32_t)(uintptr_t)t;
    threaded = 1;                                        /* before the second thread can touch malloc */
    __libc_lock();
    int r = __syscall3(SYS_thread_create, P(thread_start), P(sp), P(&errno));
    if (r < 0) { __libc_unlock(); free(stack); free(t); return -r; }
    t->tid = r;
    t->next = thrs;
    thrs = t;
    __libc_unlock();
    *tid = r;
    return 0;
}

int pthread_join(pthread_t tid, void **ret) {
    int v, r;
    while ((r = __syscall2(SYS_thread_join, tid, P(&v))) == -EINTR) {}
    if (r < 0) return -r;
    if (ret) *ret = (void *)(uintptr_t)v;
    __libc_lock();                                       /* it's gone: free its stack */
    for (struct thr **pp = &thrs; *pp; pp = &(*pp)->next)
        if ((*pp)->tid == tid) {
            struct thr *t = *pp;
            *pp = t->next;
            free(t->stack);
            free(t);
            break;
        }
    __libc_unlock();
    return 0;
}

void pthread_exit(void *ret) {
    for (;;) __syscall1(SYS_thread_exit, P(ret));        /* comes back only to run a signal handler */
}

pthread_t pthread_self(void) { return __syscall0(SYS_gettid); }
int pthread_equal(pthread_t a, pthread_t b) { return a == b; }
