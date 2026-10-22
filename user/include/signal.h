#pragma once
#include "abi.h"     /* SIGINT SIGKILL SIGTERM */
#include <sys/types.h>
typedef int sig_atomic_t;
typedef void (*sighandler_t)(int);
#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)
#define SIGABRT 6
#define SIGFPE 8
#define SIGILL 4
#define SIGSEGV 11
int kill(pid_t pid, int sig);
int raise(int sig);
/* signals are all fatal in the kernel: handlers are accepted and remembered but never run */
sighandler_t signal(int sig, sighandler_t h);
