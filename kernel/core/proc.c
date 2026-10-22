/* proc.c - user processes: ELF loading, spawn, exit, wait, signals, faults */
#include "proc.h"
#include "tty.h"

struct task *task_create_user(const char *name, const struct regs *r);
void task_start(struct task *t);
void task_unlink_free(struct task *t);

#define STACK_INIT  (64 * 1024)   /* argv + envp limit; the stack itself faults in */

/* ---- ELF32 ---- */
struct elf_hdr {
    uint8_t ident[16]; uint16_t type, machine; uint32_t version, entry, phoff, shoff, flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} PACKED;
struct elf_phdr { uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align; } PACKED;

/* copy into another address space through the identity map */
static int copy_to_dir(uint32_t dir, uint32_t va, const void *src, uint32_t n) {
    const uint8_t *s = src;
    while (n) {
        uint32_t pa = vmm_translate(dir, va);
        if (!pa) return -EFAULT;
        uint32_t chunk = MIN(n, PAGE_SIZE - (va & 0xFFF));
        memcpy((void *)pa, s, chunk);
        va += chunk; s += chunk; n -= chunk;
    }
    return 0;
}

static int load_elf(uint32_t dir, const uint8_t *img, uint32_t size, uint32_t *entry, uint32_t *end) {
    const struct elf_hdr *h = (const void *)img;
    if (size < sizeof *h || memcmp(h->ident, "\x7f" "ELF", 4) || h->ident[4] != 1 ||
        h->machine != 3 || h->type != 2 || h->phoff + h->phnum * sizeof(struct elf_phdr) > size)
        return -ENOEXEC;
    uint32_t top = 0;
    for (int i = 0; i < h->phnum; i++) {
        const struct elf_phdr *p = (const void *)(img + h->phoff + i * sizeof *p);
        if (p->type != 1 || !p->memsz) continue;                  /* PT_LOAD */
        if (p->vaddr < USER_BASE || p->vaddr + p->memsz > MMAP_TOP ||
            p->offset + p->filesz > size || p->filesz > p->memsz)
            return -ENOEXEC;
        if (vmm_alloc_range(dir, p->vaddr, p->memsz, PTE_U | PTE_W) < 0) return -ENOMEM;
        if (copy_to_dir(dir, p->vaddr, img + p->offset, p->filesz) < 0) return -EFAULT;
        top = MAX(top, p->vaddr + p->memsz);
    }
    *entry = h->entry;
    *end = ALIGN_UP(top, PAGE_SIZE);
    return 0;
}

/* lay out argc/argv/envp at the top of the new stack (Linux i386 style) */
static int build_stack(uint32_t dir, char *const argv[], char *const envp[], uint32_t *esp) {
    int argc = 0, envc = 0;
    uint32_t strbytes = 0;
    while (argv && argv[argc]) strbytes += strlen(argv[argc++]) + 1;
    while (envp && envp[envc]) strbytes += strlen(envp[envc++]) + 1;
    if (strbytes > STACK_INIT / 2 || argc + envc > 1024) return -E2BIG;
    uint32_t sp = USER_STACK_TOP - ALIGN_UP(strbytes, 16);
    uint32_t strp = sp;
    uint32_t *ptrs = kmalloc((argc + envc + 3) * 4);
    if (!ptrs) return -ENOMEM;
    uint32_t lo = ALIGN_DOWN(sp - (argc + envc + 3) * 4, 16);      /* the final sp: only these pages are mapped now */
    if (vmm_alloc_range(dir, lo, USER_STACK_TOP - lo, PTE_U | PTE_W) < 0) { kfree(ptrs); return -ENOMEM; }
    int k = 0;
    ptrs[k++] = argc;
    for (int i = 0; i < argc; i++) { uint32_t l = strlen(argv[i]) + 1; copy_to_dir(dir, strp, argv[i], l); ptrs[k++] = strp; strp += l; }
    ptrs[k++] = 0;
    for (int i = 0; i < envc; i++) { uint32_t l = strlen(envp[i]) + 1; copy_to_dir(dir, strp, envp[i], l); ptrs[k++] = strp; strp += l; }
    ptrs[k++] = 0;
    sp = ALIGN_DOWN(sp - k * 4, 16);
    int r = copy_to_dir(dir, sp, ptrs, k * 4);
    kfree(ptrs);
    *esp = sp;
    return r;
}

int proc_spawn(const char *path, char *const argv[], char *const envp[], const struct spawn_opts *o) {
    struct file *fds[3];
    for (int i = 0; i < 3; i++) fds[i] = fd_get((o && o->fd[i] >= 0) ? o->fd[i] : i);
    return proc_spawn_ex(path, argv, envp, o ? o->cwd : 0, fds, current->proc->tty);
}

/* a loaded but not yet running program image */
struct image { uint32_t dir, entry, end, esp; char name[PROC_NAME_MAX]; };

/* read path into a fresh address space with argv/envp on its stack. "#!interp [arg]" runs the
 * interpreter with argv = interp [arg] script args...  One level only. */
static int load_image(const char *path, char *const argv[], char *const envp[], struct image *im) {
    char cpath[PATH_MAX];
    int r = vfs_canon(current->proc->cwd, path, cpath, sizeof cpath);
    if (r < 0) return r;
    void *img; uint32_t size;
    if ((r = vfs_read_all(cpath, &img, &size)) < 0) return r;
    char **nargv = 0, ipath[128], iarg[128];
    const char *ln = img;
    if (size > 2 && ln[0] == '#' && ln[1] == '!') {
        uint32_t n = 2, e = 2, k = 0;
        while (e < size && e < 128 && ln[e] != '\n') e++;
        if (e == 128 && e < size && ln[e] != '\n') { kfree(img); return -ENOEXEC; }   /* line too long */
        while (n < e && (ln[n] == ' ' || ln[n] == '\t')) n++;
        while (n < e && ln[n] != ' ' && ln[n] != '\t' && ln[n] != '\r') ipath[k++] = ln[n++];
        ipath[k] = 0;
        while (n < e && (ln[n] == ' ' || ln[n] == '\t')) n++;
        for (k = 0; n < e && ln[n] != '\r'; ) iarg[k++] = ln[n++];
        while (k && (iarg[k - 1] == ' ' || iarg[k - 1] == '\t')) k--;
        iarg[k] = 0;
        kfree(img);
        if (!ipath[0]) return -ENOEXEC;
        int ac = 0;
        while (argv && argv[ac]) ac++;
        nargv = kmalloc((ac + 4) * sizeof *nargv);
        if (!nargv) return -ENOMEM;
        k = 0;
        nargv[k++] = ipath;
        if (iarg[0]) nargv[k++] = iarg;
        nargv[k++] = cpath;
        for (int i = 1; i < ac; i++) nargv[k++] = argv[i];
        nargv[k] = 0;
        argv = nargv;
        char ip2[PATH_MAX];
        if ((r = vfs_canon(current->proc->cwd, ipath, ip2, sizeof ip2)) < 0 ||
            (r = vfs_read_all(ip2, &img, &size)) < 0) { kfree(nargv); return r; }
    }
    im->dir = vmm_new_dir();
    if (!im->dir) { kfree(img); kfree(nargv); return -ENOMEM; }
    r = load_elf(im->dir, img, size, &im->entry, &im->end);
    kfree(img);
    if (r == 0) r = build_stack(im->dir, argv, envp, &im->esp);
    kfree(nargv);
    if (r < 0) { vmm_free_dir(im->dir); return r; }
    const char *base = strrchr(cpath, '/');
    strlcpy(im->name, base ? base + 1 : cpath, sizeof im->name);
    return 0;
}

/* a fresh ring-3 register frame starting at eip with stack esp */
static void user_frame(struct regs *fr, uint32_t eip, uint32_t esp) {
    memset(fr, 0, sizeof *fr);
    fr->gs = fr->fs = fr->es = fr->ds = fr->ss = USER_DS;
    fr->cs = USER_CS;
    fr->eip = eip;
    fr->useresp = esp;
    fr->eflags = 0x202;
}

int proc_spawn_ex(const char *path, char *const argv[], char *const envp[], const char *cwd,
                  struct file *fds[3], struct tty *tty) {
    struct image im;
    int r = load_image(path, argv, envp, &im);
    if (r < 0) return r;
    struct regs fr;
    user_frame(&fr, im.entry, im.esp);
    struct task *t = task_create_user(im.name, &fr);
    if (!t) { vmm_free_dir(im.dir); return -ENOMEM; }
    t->dir = im.dir;
    t->brk_start = t->brk = im.end;
    t->tty = tty;
    for (int i = 1; i < NSIG; i++)                       /* like exec: ignored stays ignored */
        if (current->proc->sig_handler[i] == 1) t->sig_handler[i] = 1;
    char cw[PATH_MAX];
    if (cwd && vfs_canon(current->proc->cwd, cwd, cw, sizeof cw) >= 0) strlcpy(t->cwd, cw, sizeof t->cwd);
    else strlcpy(t->cwd, current->proc->cwd, sizeof t->cwd);
    for (int i = 0; i < 3; i++)
        if (fds[i]) { fds[i]->refs++; t->fds[i] = fds[i]; }
    task_start(t);
    return t->pid;
}

/* SIGKILL the other threads of current's process; the leader then waits until they're all zombies.
 * They die on their way back to ring 3, so a thread busy in the kernel finishes that first. */
static void kill_other_threads(void) {
    struct task *p = current->proc;
    uint32_t f = irq_save();
    for (struct task *t = task_list(); t; t = t->next)
        if (t->proc == p && t != current && t->state != PS_ZOMBIE) {
            t->sig_pending |= 1u << SIGKILL;
            if (t->state == PS_BLOCKED) t->state = PS_READY;
        }
    while (current == p && p->nthreads > 1)
        if (sleep_on(p, 0) < 0) yield();                 /* a signal of our own pending: just let them run */
    irq_restore(f);
}

/* Replace current's image. Everything that can fail happens before the old image is touched. */
int proc_exec(const char *path, char *const argv[], char *const envp[], struct regs *r) {
    struct task *p = current->proc;
    if (current != p) return -EINVAL;                    /* ponytail: only the main thread may exec */
    struct image im;
    int e = load_image(path, argv, envp, &im);
    if (e < 0) return e;
    /* point of no return */
    if (p->nthreads > 1) {
        p->exiting = true;               /* the threads die quietly instead of taking us with them */
        kill_other_threads();
        p->exiting = false;
    }
    for (int i = 0; i < MAX_FDS; i++)
        if (p->fds[i] && (p->fd_cloexec >> i & 1)) { vfs_close(p->fds[i]); p->fds[i] = 0; }
    p->fd_cloexec = 0;
    cli();
    uint32_t old = p->dir;
    p->dir = im.dir;
    write_cr3(im.dir);
    vmm_free_dir(old);
    sti();
    uvm_free(p);
    p->brk_start = p->brk = im.end;
    p->uerrno = 0;
    strlcpy(p->name, im.name, sizeof p->name);
    for (int i = 1; i < NSIG; i++)                       /* handlers lived in the old image; ignored stays ignored */
        if (p->sig_handler[i] > 1) p->sig_handler[i] = 0;
    p->sig_restorer = 0;
    __asm__ volatile("fninit");
    user_frame(r, im.entry, im.esp);
    return 0;
}

static void close_all(struct task *t) {
    for (int i = 0; i < MAX_FDS; i++)
        if (t->fds[i]) { vfs_close(t->fds[i]); t->fds[i] = 0; }
    t->fd_cloexec = 0;
}

/* a thread (never the leader) leaves; whoever joins it, or the idle loop once the process is gone, frees it */
static NORETURN void thread_die(int value) {
    struct task *t = current, *p = t->proc;
    cli();
    t->exit_code = value;
    t->state = PS_ZOMBIE;
    p->nthreads--;
    wakeup(p);
    schedule();
    panic("zombie thread %d scheduled", t->pid);
}

/* exit the whole process: any thread may call it, the leader does the teardown once it's alone */
NORETURN void proc_exit(int code) {
    struct task *t = current, *p = t->proc;
    cli();
    bool first = !p->exiting;            /* the first thread out sets the status and kills the rest */
    if (first) { p->exiting = true; p->exit_code = code; }
    t->sig_pending = 0;                  /* and proc_kill ignores an exiting process from here on */
    sti();                               /* closing pipes/files may sleep; allow IRQs meanwhile */
    if (p->nthreads > 1 && (first || t == p)) kill_other_threads();
    if (t != p) thread_die(0);
    close_all(t);
    uvm_free(t);
    cli();
    if (t->user) {
        uint32_t d = t->dir;
        t->dir = vmm_kernel_dir();
        write_cr3(t->dir);
        vmm_free_dir(d);
    }
    code = t->exit_code;
    if (code) kprintf("[proc] %s[%d] exited with %d\n", t->name, t->pid, code);
    struct task *parent = task_find(t->ppid);
    for (struct task *c = task_list(); c; c = c->next)    /* orphan our children (and unjoined threads) */
        if (c->ppid == t->pid) { c->ppid = 0; c->orphan = true; }
    /* kernel threads never wait(): their children are reaped by the idle loop */
    if (!parent || parent->state == PS_ZOMBIE || !parent->user) t->orphan = true;
    if (t->tty) tty_proc_exited(t->tty, t->pid);
    t->state = PS_ZOMBIE;
    if (parent) wakeup(parent);
    if (parent && parent->user && parent->state != PS_ZOMBIE) proc_kill(parent->pid, SIGCHLD);
    schedule();
    panic("zombie %d scheduled", t->pid);
}

void kthread_exit(int code) { proc_exit(code); }

void proc_reap_orphans(void) {
    uint32_t f = irq_save();
    for (struct task *t = task_list(); t; ) {
        struct task *n = t->next;
        if (t->state == PS_ZOMBIE && t->orphan && t != current) task_unlink_free(t);
        t = n;
    }
    irq_restore(f);
}

int proc_wait(int pid, int *status, int flags) {
    struct task *me = current->proc;
    uint32_t f = irq_save();
    for (;;) {
        bool have = false;
        for (struct task *t = task_list(); t; t = t->next) {
            if (t->ppid != me->pid || t->proc != t || (pid > 0 && t->pid != pid)) continue;
            have = true;
            if (t->state != PS_ZOMBIE) continue;
            int p = t->pid;
            if (status) *status = t->exit_code;
            task_unlink_free(t);
            irq_restore(f);
            return p;
        }
        if (!have) { irq_restore(f); return -ECHILD; }
        if (flags & WNOHANG) { irq_restore(f); return 0; }
        if (sleep_on(me, 0) < 0) { irq_restore(f); return -EINTR; }
    }
}

/* ---- threads: tasks sharing the leader's address space, fds, cwd and signal handlers ---- */

int proc_thread_create(uint32_t entry, uint32_t stack, uint32_t errno_addr) {
    struct task *p = current->proc;
    if (!p->user || p->exiting) return -EINVAL;
    if (!vmm_user_ok(p->dir, entry, 1, false) || stack < USER_BASE || stack > USER_STACK_TOP) return -EFAULT;
    /* aligned, so it never straddles a page; the scheduler skips it if it's unmapped later */
    if (errno_addr && ((errno_addr & 3) || !vmm_user_ok(p->dir, errno_addr, 4, true))) return -EFAULT;
    struct regs fr;
    user_frame(&fr, entry, stack);
    struct task *t = task_create_user(p->name, &fr);
    if (!t) return -ENOMEM;
    t->proc = p;
    t->dir = p->dir;
    t->ppid = p->pid;                    /* listed under the process in ps; wait() skips threads */
    t->sig_blocked = current->sig_blocked;
    if (errno_addr && !p->uerrno) {      /* from now on errno is per thread */
        p->uerrno = errno_addr;
        current->uerrno_val = *(int *)errno_addr;
    }
    p->nthreads++;
    task_start(t);
    return t->pid;
}

/* returns only to let a signal handler run (libc calls it again) */
void proc_thread_exit(int value) {
    struct task *p = current->proc;
    if (current != p) thread_die(value);
    /* the main thread: the process lives on until the last thread is gone, then exits with 0 */
    uint32_t f = irq_save();
    while (p->nthreads > 1 && !p->exiting)
        if (sleep_on(p, 0) < 0) { irq_restore(f); return; }
    irq_restore(f);
    proc_exit(0);
}

int proc_thread_join(int tid, int *value) {
    struct task *p = current->proc;
    uint32_t f = irq_save();
    for (;;) {
        struct task *t = task_find(tid);
        if (!t || t->proc != p || t == p) { irq_restore(f); return -ESRCH; }  /* the main thread can't be joined */
        if (t == current) { irq_restore(f); return -EINVAL; }
        if (t->state == PS_ZOMBIE) {
            if (value) *value = t->exit_code;
            task_unlink_free(t);
            irq_restore(f);
            return 0;
        }
        if (sleep_on(p, 0) < 0) { irq_restore(f); return -EINTR; }
    }
}

/* ---- signals ---- */

#define SIG_IGN_ 1
static bool ignored(struct task *t, int sig) {
    uint32_t h = t->proc->sig_handler[sig];
    return sig != SIGKILL && (h == SIG_IGN_ || (!h && (sig == SIGCHLD || sig == SIGWINCH)));
}

int proc_kill(int pid, int sig) {
    struct task *t = task_find(pid);
    if (!t || pid == 0 || t->state == PS_ZOMBIE) return -ESRCH;
    if (!t->user) return -EPERM;
    if (sig < 0 || sig >= NSIG) return -EINVAL;
    if (sig == 0 || ignored(t, sig) || t->proc->exiting) return 0;
    t->sig_pending |= 1u << sig;
    if (t->state == PS_BLOCKED && sig_deliverable(t)) t->state = PS_READY;
    return 0;
}

int proc_sigaction(int sig, uint32_t handler, uint32_t restorer) {
    if (sig < 1 || sig >= NSIG || sig == SIGKILL) return -EINVAL;
    if (handler > SIG_IGN_ && !vmm_user_ok(current->dir, handler, 1, false)) return -EFAULT;
    uint32_t old = current->proc->sig_handler[sig];
    current->proc->sig_handler[sig] = handler;
    if (restorer) current->proc->sig_restorer = restorer;
    if (ignored(current, sig)) current->sig_pending &= ~(1u << sig);
    return old;
}

int proc_sigprocmask(int how, uint32_t set) {
    uint32_t old = current->sig_blocked;
    set &= ~(1u << SIGKILL | 1);
    if (how == 0) current->sig_blocked |= set;
    else if (how == 1) current->sig_blocked &= ~set;
    else if (how == 2) current->sig_blocked = set;
    else return -EINVAL;
    return old;
}

/* what a handler sees below its return address and argument; sigreturn puts it all back */
struct sigframe {
    uint32_t ret, sig;
    uint32_t eax, ebx, ecx, edx, esi, edi, ebp, esp, eip, eflags, mask;
    uint8_t fpu[108];
};

/* map any missing stack pages under [va, va+len): the frame may land below the deepest page so far */
static bool stack_ok(uint32_t va, uint32_t len) {
    if (va < USER_STACK_TOP - STACK_MAX || va + len > USER_STACK_TOP) return false;
    return vmm_alloc_range(current->dir, ALIGN_DOWN(va, PAGE_SIZE), va + len - ALIGN_DOWN(va, PAGE_SIZE), PTE_U | PTE_W) == 0;
}

void proc_deliver_signals(struct regs *r) {
    struct task *t = current;
    uint32_t m;
    while ((m = sig_deliverable(t))) {
        int sig = __builtin_ctz(m);
        t->sig_pending &= ~(1u << sig);
        uint32_t h = t->proc->sig_handler[sig];
        if (ignored(t, sig)) continue;
        if (!h || sig == SIGKILL || !t->proc->sig_restorer) proc_exit(128 + sig);
        struct sigframe f = { t->proc->sig_restorer, sig, r->eax, r->ebx, r->ecx, r->edx, r->esi, r->edi, r->ebp,
                              r->useresp, r->eip, r->eflags, t->sig_blocked };
        uint8_t fpu[108] __attribute__((aligned(16)));
        __asm__ volatile("fnsave %0" : "=m"(fpu));       /* also resets the FPU for the handler */
        memcpy(f.fpu, fpu, sizeof fpu);
        uint32_t sp = ((r->useresp - sizeof f) & ~15u) - 4;   /* &f.sig 16-aligned, like after a call */
        if (!vmm_user_ok(t->dir, sp, sizeof f, true) && !stack_ok(sp, sizeof f)) proc_exit(128 + SIGSEGV);
        memcpy((void *)sp, &f, sizeof f);
        t->sig_blocked |= 1u << sig;                     /* no re-entry until it returns */
        r->useresp = sp;
        r->eip = h;
        return;
    }
}

void proc_sigreturn(struct regs *r) {
    uint32_t sp = r->useresp - 4;                        /* the handler's ret popped f.ret */
    if (!vmm_user_ok(current->dir, sp, sizeof(struct sigframe), false)) proc_exit(128 + SIGSEGV);
    struct sigframe f;
    memcpy(&f, (void *)sp, sizeof f);
    r->eax = f.eax; r->ebx = f.ebx; r->ecx = f.ecx; r->edx = f.edx;
    r->esi = f.esi; r->edi = f.edi; r->ebp = f.ebp; r->useresp = f.esp; r->eip = f.eip;
    r->eflags = (f.eflags & 0xCD5) | 0x202;              /* arithmetic flags + DF only; IF on, IOPL 0 */
    current->sig_blocked = f.mask & ~(1u << SIGKILL);
    uint8_t fpu[108] __attribute__((aligned(16)));
    memcpy(fpu, f.fpu, sizeof fpu);
    __asm__ volatile("frstor %0" :: "m"(fpu));
}

int fd_alloc(struct file *f) {
    for (int i = 0; i < MAX_FDS; i++)
        if (!current->proc->fds[i]) { current->proc->fds[i] = f; current->proc->fd_cloexec &= ~(1u << i); return i; }
    return -EMFILE;
}

struct file *fd_get(int fd) {
    return (fd >= 0 && fd < MAX_FDS) ? current->proc->fds[fd] : 0;
}

/* page faults and other exceptions from ring 3; a lazy user page touched by the kernel is mapped too */
bool proc_handle_fault(struct regs *r) {
    uint32_t addr = read_cr2();
    bool user = (r->cs & 3) == 3;
    if (r->int_no == 14 && !(r->err & 1) && uvm_fault(addr, r->err & 2, user ? r->useresp : 0) == 0) return true;
    if (!user) return false;
    int sig = r->int_no == 14 ? SIGSEGV : r->int_no == 0 ? SIGFPE : SIGILL;
    if (current->proc->sig_handler[sig] > SIG_IGN_ && current->proc->sig_restorer && !(current->sig_blocked >> sig & 1)) {
        current->sig_pending |= 1u << sig;                 /* a handler: sched_irq_exit runs it */
        return true;
    }
    static const char *names[] = { [0] = "divide by zero", [6] = "illegal instruction",
                                   [13] = "general protection fault", [14] = "segmentation fault" };
    const char *what = r->int_no < ARRAY_LEN(names) && names[r->int_no] ? names[r->int_no] : "crashed";
    char msg[128];
    if (r->int_no == 14)
        snprintf(msg, sizeof msg, "\x1b[31m%s[%d]: %s at %08x (eip %08x)\x1b[0m\n", current->name, current->pid, what, addr, r->eip);
    else
        snprintf(msg, sizeof msg, "\x1b[31m%s[%d]: %s (eip %08x)\x1b[0m\n", current->name, current->pid, what, r->eip);
    kprintf("%s", msg);
    if (current->proc->tty) tty_output(current->proc->tty, msg, strlen(msg));
    sti();
    proc_exit(128 + sig);
}
