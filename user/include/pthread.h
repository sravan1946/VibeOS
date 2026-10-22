/* pthread.h - kernel threads sharing the process (SYS_thread_*), futex-based mutexes and condition
 * variables. malloc and stdio's write side are locked once a thread exists; errno is per thread. */
#pragma once
#include <stddef.h>
#include <sys/types.h>

typedef int pthread_t;                                   /* the thread id (gettid) */
typedef struct { size_t stacksize; } pthread_attr_t;
typedef struct { volatile int v; } pthread_mutex_t;     /* 0 free, 1 locked, 2 locked with waiters */
typedef int pthread_mutexattr_t;
typedef struct { volatile int seq; } pthread_cond_t;
typedef int pthread_condattr_t;
typedef struct { volatile int state; } pthread_once_t;

#define PTHREAD_MUTEX_INITIALIZER { 0 }
#define PTHREAD_COND_INITIALIZER { 0 }
#define PTHREAD_ONCE_INIT { 0 }
#define PTHREAD_STACK_MIN 16384

/* all return 0 or an errno value (not -1); stacks come from malloc, 128 KiB unless attr says otherwise */
int pthread_create(pthread_t *t, const pthread_attr_t *attr, void *(*fn)(void *), void *arg);
int pthread_join(pthread_t t, void **ret);
void pthread_exit(void *ret) __attribute__((noreturn));  /* in main: the process exits 0 after the last thread */
pthread_t pthread_self(void);
int pthread_equal(pthread_t a, pthread_t b);
int pthread_attr_init(pthread_attr_t *a);
int pthread_attr_destroy(pthread_attr_t *a);
int pthread_attr_setstacksize(pthread_attr_t *a, size_t n);
int pthread_attr_getstacksize(const pthread_attr_t *a, size_t *n);

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr);
int pthread_mutex_destroy(pthread_mutex_t *m);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_trylock(pthread_mutex_t *m);            /* EBUSY if held */
int pthread_mutex_unlock(pthread_mutex_t *m);

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *attr);
int pthread_cond_destroy(pthread_cond_t *c);
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);   /* may wake spuriously: wait in a loop */
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);

int pthread_once(pthread_once_t *o, void (*fn)(void));

/* raw: sleep while *addr == val / wake everyone sleeping on addr */
int futex_wait(volatile int *addr, int val);
int futex_wake(volatile int *addr);
