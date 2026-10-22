/* proc.h - tasks, scheduler, processes */
#pragma once
#include "arch.h"
#include "vfs.h"

#define MAX_FDS 32
#define KSTACK_SIZE 16384

struct tty;

/* A process is its leader task plus any threads (tasks with proc = leader). Threads share the
 * page directory; the per-process fields below are only meaningful in the leader, so always
 * reach them through t->proc. */
struct task {
    int pid, ppid;                /* a thread's pid is its tid; ppid = its leader's pid */
    int state;                    /* PS_* from abi.h */
    char name[PROC_NAME_MAX];
    uint32_t kstack;              /* kmalloc'd base */
    uint32_t kesp;                /* saved by context_switch */
    uint32_t dir;                 /* page directory (kernel dir for kthreads) */
    bool user;
    struct task *proc;            /* the leader (itself for a process) */
    /* ---- per process (leader only) ---- */
    uint32_t brk_start, brk;      /* [brk_start, brk) faults in zeroed pages */
    struct vma *vmas;             /* mmap'd ranges, sorted (mm/mmap.c) */
    struct file *fds[MAX_FDS];
    uint32_t fd_cloexec;          /* bit n: close fds[n] on exec */
    char cwd[PATH_MAX];
    struct tty *tty;              /* controlling terminal */
    uint32_t sig_handler[NSIG];   /* 0 default, 1 ignore, else a user address */
    uint32_t sig_restorer;
    int nthreads;                 /* live tasks in the process, leader included */
    bool exiting;                 /* exit()/fatal signal under way: exit_code is final */
    uint32_t uerrno;              /* user address of libc's errno, swapped per thread (0 = unthreaded) */
    /* ---- per task ---- */
    int uerrno_val;               /* this thread's errno while it isn't running */
    void *chan;                   /* sleeping on */
    uint64_t wake_at;             /* timed sleep deadline (0 = none) */
    int exit_code;                /* leader: the process status; thread: its thread_exit value */
    uint32_t sig_pending, sig_blocked;   /* bit n = signal n; per thread */
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
int  proc_exec(const char *path, char *const argv[], char *const envp[], struct regs *r);   /* returns only on error */
int  proc_thread_create(uint32_t entry, uint32_t stack, uint32_t errno_addr);
void proc_thread_exit(int value);            /* returns only so a signal handler can run */
int  proc_thread_join(int tid, int *value);
int  proc_kill(int pid, int sig);
/* a signal that will interrupt this task (pending and not blocked; SIGKILL can't be blocked) */
static inline uint32_t sig_deliverable(struct task *t) { return t->sig_pending & ~(t->sig_blocked & ~(1u << SIGKILL)); }
void proc_deliver_signals(struct regs *r);   /* on the way back to ring 3 */
void proc_sigreturn(struct regs *r);
int  proc_sigaction(int sig, uint32_t handler, uint32_t restorer);
int  proc_sigprocmask(int how, uint32_t set);
int  fd_alloc(struct file *f);                     /* in current; -EMFILE */
struct file *fd_get(int fd);
void proc_reap_orphans(void);

/* mmap (mm/mmap.c): ranges of current's process; uvm_free drops p's list (its pages go with the page directory) */
int  uvm_mmap(uint32_t addr, uint32_t len, int prot, int flags, int fd, uint32_t off);
int  uvm_munmap(uint32_t addr, uint32_t len);
int  uvm_mprotect(uint32_t addr, uint32_t len, int prot);
uint32_t uvm_brk_limit(void);                      /* sbrk can't grow past the lowest mapping */
void uvm_free(struct task *p);

/* syscalls (core/syscall.c) */
void syscall_init(void);
