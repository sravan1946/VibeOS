/* proc.c - user processes: ELF loading, spawn, exit, wait, signals, faults */
#include "proc.h"
#include "tty.h"

struct task *task_create_user(const char *name, const struct regs *r);
void task_start(struct task *t);
void task_unlink_free(struct task *t);

#define STACK_INIT  (64 * 1024)
#define STACK_MAX   (8u << 20)

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
        if (p->vaddr < USER_BASE || p->vaddr + p->memsz > USER_STACK_TOP - STACK_MAX ||
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
    return proc_spawn_ex(path, argv, envp, o ? o->cwd : 0, fds, current->tty);
}

int proc_spawn_ex(const char *path, char *const argv[], char *const envp[], const char *cwd,
                  struct file *fds[3], struct tty *tty) {
    char cpath[PATH_MAX];
    int r = vfs_canon(current->cwd, path, cpath, sizeof cpath);
    if (r < 0) return r;
    void *img; uint32_t size;
    if ((r = vfs_read_all(cpath, &img, &size)) < 0) return r;
    uint32_t dir = vmm_new_dir(), entry = 0, end = 0, esp = 0;
    if (!dir) { kfree(img); return -ENOMEM; }
    r = load_elf(dir, img, size, &entry, &end);
    kfree(img);
    if (r == 0 && vmm_alloc_range(dir, USER_STACK_TOP - STACK_INIT, STACK_INIT, PTE_U | PTE_W) < 0) r = -ENOMEM;
    if (r == 0) r = build_stack(dir, argv, envp, &esp);
    if (r < 0) { vmm_free_dir(dir); return r; }

    struct regs fr;
    memset(&fr, 0, sizeof fr);
    fr.gs = fr.fs = fr.es = fr.ds = fr.ss = USER_DS;
    fr.cs = USER_CS;
    fr.eip = entry;
    fr.useresp = esp;
    fr.eflags = 0x202;
    const char *base = strrchr(cpath, '/');
    struct task *t = task_create_user(base ? base + 1 : cpath, &fr);
    if (!t) { vmm_free_dir(dir); return -ENOMEM; }
    t->dir = dir;
    t->brk_start = t->brk = end;
    t->tty = tty;
    char cw[PATH_MAX];
    if (cwd && vfs_canon(current->cwd, cwd, cw, sizeof cw) >= 0) strlcpy(t->cwd, cw, sizeof t->cwd);
    else strlcpy(t->cwd, current->cwd, sizeof t->cwd);
    for (int i = 0; i < 3; i++)
        if (fds[i]) { fds[i]->refs++; t->fds[i] = fds[i]; }
    task_start(t);
    return t->pid;
}

static void close_all(struct task *t) {
    for (int i = 0; i < MAX_FDS; i++)
        if (t->fds[i]) { vfs_close(t->fds[i]); t->fds[i] = 0; }
}

NORETURN void proc_exit(int code) {
    struct task *t = current;
    cli();
    sti();                       /* closing pipes/files may sleep; allow IRQs meanwhile */
    close_all(t);
    cli();
    if (t->user) {
        uint32_t d = t->dir;
        t->dir = vmm_kernel_dir();
        write_cr3(t->dir);
        vmm_free_dir(d);
    }
    t->exit_code = code;
    if (code) kprintf("[proc] %s[%d] exited with %d\n", t->name, t->pid, code);
    struct task *parent = task_find(t->ppid);
    for (struct task *c = task_list(); c; c = c->next)    /* orphan our children */
        if (c->ppid == t->pid) { c->ppid = 0; c->orphan = true; }
    /* kernel threads never wait(): their children are reaped by the idle loop */
    if (!parent || parent->state == PS_ZOMBIE || !parent->user) t->orphan = true;
    if (t->tty) tty_proc_exited(t->tty, t->pid);
    t->state = PS_ZOMBIE;
    if (parent) wakeup(parent);
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
    uint32_t f = irq_save();
    for (;;) {
        bool have = false;
        for (struct task *t = task_list(); t; t = t->next) {
            if (t->ppid != current->pid || (pid > 0 && t->pid != pid)) continue;
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
        if (sleep_on(current, 0) < 0) { irq_restore(f); return -EINTR; }
    }
}

int proc_kill(int pid, int sig) {
    struct task *t = task_find(pid);
    if (!t || pid == 0 || t->state == PS_ZOMBIE) return -ESRCH;
    if (!t->user) return -EPERM;
    if (sig == 0) return 0;
    t->pending_sig = sig;
    if (t->state == PS_BLOCKED) t->state = PS_READY;
    return 0;
}

int fd_alloc(struct file *f) {
    for (int i = 0; i < MAX_FDS; i++)
        if (!current->fds[i]) { current->fds[i] = f; return i; }
    return -EMFILE;
}

struct file *fd_get(int fd) {
    return (fd >= 0 && fd < MAX_FDS) ? current->fds[fd] : 0;
}

/* page faults and other exceptions from ring 3 */
bool proc_handle_fault(struct regs *r) {
    if ((r->cs & 3) != 3) return false;
    uint32_t addr = read_cr2();
    if (r->int_no == 14 && !(r->err & 1) && addr < USER_STACK_TOP &&
        addr >= USER_STACK_TOP - STACK_MAX && addr + 65536 >= r->useresp) {
        if (vmm_alloc_range(current->dir, ALIGN_DOWN(addr, PAGE_SIZE), PAGE_SIZE, PTE_U | PTE_W) == 0)
            return true;                                   /* stack growth */
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
    if (current->tty) tty_output(current->tty, msg, strlen(msg));
    sti();
    proc_exit(128 + (r->int_no == 14 ? 11 : r->int_no == 0 ? 8 : 4));
}
