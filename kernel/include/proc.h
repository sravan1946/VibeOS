/* proc.h - tasks, scheduler, processes */
#pragma once
#include "arch.h"
#include "vfs.h"

#define MAX_FDS 32
#define KSTACK_SIZE 16384

struct tty;

struct task {
    int pid, ppid;
    int state;                    /* PS_* from abi.h */
    char name[PROC_NAME_MAX];
    uint32_t kstack;              /* kmalloc'd base */
    uint32_t kesp;                /* saved by context_switch */
    uint32_t dir;                 /* page directory (kernel dir for kthreads) */
    bool user;
    uint32_t brk_start, brk;
    struct file *fds[MAX_FDS];
    char cwd[PATH_MAX];
    struct tty *tty;              /* controlling terminal */
    void *chan;                   /* sleeping on */
    uint64_t wake_at;             /* timed sleep deadline (0 = none) */
    int exit_code;
    int pending_sig;
    bool orphan;                  /* no parent will reap it */
    uint32_t cpu_ms;
    struct task *next;            /* all-tasks list */
    uint8_t fpu[108] __attribute__((aligned(16)));
};

extern struct task *current;

void sched_init(void);
struct task *kthread_create(const char *name, int (*fn)(void *), void *arg);
void schedule(void);
void yield(void);
/* Block on chan; caller must have interrupts disabled and re-check its condition
 * in a loop. Returns -EINTR if a signal is pending, 0 otherwise. Timeout in ms (0 = none). */
int  sleep_on(void *chan, uint32_t timeout_ms);
void wakeup(void *chan);
void sleep_ms(uint32_t ms);
struct task *task_find(int pid);
struct task *task_list(void);
uint32_t idle_ms(void);

/* processes (core/proc.c) */
int  proc_spawn(const char *path, char *const argv[], char *const envp[],
                const struct spawn_opts *o);       /* kernel pointers; fds from current */
int  proc_spawn_ex(const char *path, char *const argv[], char *const envp[], const char *cwd,
                   struct file *fds[3], struct tty *tty);   /* fds may be NULL entries */
NORETURN void proc_exit(int code);
int  proc_wait(int pid, int *status, int flags);
int  proc_kill(int pid, int sig);
int  fd_alloc(struct file *f);                     /* in current; -EMFILE */
struct file *fd_get(int fd);
void proc_reap_orphans(void);

/* syscalls (core/syscall.c) */
void syscall_init(void);
