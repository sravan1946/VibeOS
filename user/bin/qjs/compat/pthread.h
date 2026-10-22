/* pthread.h - single-threaded stand-ins for QuickJS (Atomics.wait, the class-id lock, os.Worker).
 * Locks are no-ops; pthread_create fails, so `new os.Worker()` throws instead of running. */
#pragma once
#include <errno.h>
#include <time.h>
typedef int pthread_t, pthread_attr_t, pthread_mutex_t, pthread_cond_t, pthread_mutexattr_t, pthread_condattr_t;
#define PTHREAD_MUTEX_INITIALIZER 0
#define PTHREAD_COND_INITIALIZER 0
#define PTHREAD_CREATE_DETACHED 1
static inline int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a) { (void)a; *m = 0; return 0; }
static inline int pthread_mutex_destroy(pthread_mutex_t *m) { (void)m; return 0; }
static inline int pthread_mutex_lock(pthread_mutex_t *m) { (void)m; return 0; }
static inline int pthread_mutex_unlock(pthread_mutex_t *m) { (void)m; return 0; }
static inline int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a) { (void)a; *c = 0; return 0; }
static inline int pthread_cond_destroy(pthread_cond_t *c) { (void)c; return 0; }
static inline int pthread_cond_signal(pthread_cond_t *c) { (void)c; return 0; }
static inline int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) { (void)c; (void)m; return EINVAL; }
static inline int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *t) { (void)c; (void)m; (void)t; return ETIMEDOUT; }
static inline int pthread_attr_init(pthread_attr_t *a) { *a = 0; return 0; }
static inline int pthread_attr_destroy(pthread_attr_t *a) { (void)a; return 0; }
static inline int pthread_attr_setdetachstate(pthread_attr_t *a, int s) { (void)a; (void)s; return 0; }
static inline int pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*f)(void *), void *arg) { (void)t; (void)a; (void)f; (void)arg; return EAGAIN; }
