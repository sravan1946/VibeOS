#pragma once
#include "abi.h"     /* SIG* numbers, NSIG */
#include <sys/types.h>
typedef int sig_atomic_t;
typedef void (*sighandler_t)(int);
typedef uint32_t sigset_t;
#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2
/* sa_mask and sa_flags are accepted but ignored: the running signal is blocked during its
 * handler, nothing else; interrupted syscalls fail with EINTR (no SA_RESTART) */
struct sigaction { sighandler_t sa_handler; sigset_t sa_mask; int sa_flags; };
#define SA_RESTART 0x10000000
int kill(pid_t pid, int sig);
int raise(int sig);
sighandler_t signal(int sig, sighandler_t h);
int sigaction(int sig, const struct sigaction *sa, struct sigaction *old);
int sigprocmask(int how, const sigset_t *set, sigset_t *old);
int sigemptyset(sigset_t *s);
int sigfillset(sigset_t *s);
int sigaddset(sigset_t *s, int n);
int sigdelset(sigset_t *s, int n);
int sigismember(const sigset_t *s, int n);
