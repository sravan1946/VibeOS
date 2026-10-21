/* syscall.c - int 0x80 dispatch; see common/abi.h for the contract */
#include "proc.h"
#include "tty.h"
#include "net.h"

int pipe_create(struct file **rd, struct file **wr);
uint32_t rtc_now(void);
size_t klog_read(char *buf, size_t off, size_t n);
size_t klog_size(void);
int win_syscall(int num, uint32_t a, uint32_t b, uint32_t c);   /* gui/wm.c */
char poll_chan;                                                  /* woken by every wakeup() */

/* ---- user memory checks ---- */
static bool uok(const void *p, uint32_t n, bool w) {
    return n == 0 || vmm_user_ok(current->dir, (uint32_t)p, n, w);
}

/* copy a NUL-terminated user string; -EFAULT / -ENAMETOOLONG */
static int ustr(const char *u, char *k, size_t max) {
    for (size_t i = 0; i < max; i++) {
        if (((uint32_t)(u + i) & 0xFFF) == 0 || i == 0)
            if (!uok(u + i, 1, false)) return -EFAULT;
        if (!(k[i] = u[i])) return 0;
    }
    return -ENAMETOOLONG;
}

#define UPATH(var, src) char var[PATH_MAX]; { int _r = ustr((const char *)(src), var, sizeof var); if (_r < 0) return _r; }

/* copy a NULL-terminated user string vector into one kmalloc block */
static char **uvec(char *const *u, int *err) {
    *err = 0;
    if (!u) return 0;
    int n = 0;
    size_t bytes = 0;
    for (;; n++) {
        if (n > 512 || !uok(&u[n], 4, false)) { *err = -E2BIG; return 0; }
        if (!u[n]) break;
        char tmp[4096];
        int r = ustr(u[n], tmp, sizeof tmp);
        if (r < 0) { *err = r; return 0; }
        bytes += strlen(tmp) + 1;
    }
    char **v = kmalloc((n + 1) * sizeof(char *) + bytes);
    if (!v) { *err = -ENOMEM; return 0; }
    char *s = (char *)(v + n + 1);
    for (int i = 0; i < n; i++) { v[i] = s; strcpy(s, u[i]); s += strlen(s) + 1; }
    v[n] = 0;
    return v;
}

static int sys_path1(int num, const char *up) {
    UPATH(p, up);
    if (num == SYS_mkdir) return vfs_mkdir(current->cwd, p);
    return vfs_unlink(current->cwd, p, num == SYS_rmdir);
}

static int sys_rename(const char *ua, const char *ub) {
    UPATH(p, ua);
    UPATH(q, ub);
    return vfs_rename(current->cwd, p, q);
}

static int sys_open(const char *up, int flags) {
    UPATH(p, up);
    struct file *f;
    int r = vfs_open(current->cwd, p, flags, &f);
    if (r < 0) return r;
    if ((r = fd_alloc(f)) < 0) vfs_close(f);
    return r;
}

static int sys_close(int fd) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    current->fds[fd] = 0;
    vfs_close(f);
    return 0;
}

static int sys_rw(int fd, void *buf, uint32_t n, bool w) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    if (!uok(buf, n, !w)) return -EFAULT;
    return w ? vfs_write(f, buf, n) : vfs_read(f, buf, n);
}

static int sys_stat(const char *up, struct stat *st) {
    UPATH(p, up);
    if (!uok(st, sizeof *st, true)) return -EFAULT;
    struct vnode *v;
    int r = vfs_lookup(current->cwd, p, &v);
    if (r < 0) return r;
    vfs_stat_vnode(v, st);
    vnode_put(v);
    return 0;
}

static int sys_fstat(int fd, struct stat *st) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    if (!uok(st, sizeof *st, true)) return -EFAULT;
    return vfs_stat_vnode(f->vn, st);
}

static int sys_readdir(int fd, struct dirent *ud) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    if (!S_ISDIR(f->vn->mode) || !f->vn->ops->readdir) return -ENOTDIR;
    if (!uok(ud, sizeof *ud, true)) return -EFAULT;
    return f->vn->ops->readdir(f->vn, &f->dir_cookie, ud);
}

static int sys_chdir(const char *up) {
    UPATH(p, up);
    char c[PATH_MAX];
    int r = vfs_canon(current->cwd, p, c, sizeof c);
    if (r < 0) return r;
    struct vnode *v;
    if ((r = vfs_lookup("/", c, &v)) < 0) return r;
    bool dir = S_ISDIR(v->mode);
    vnode_put(v);
    if (!dir) return -ENOTDIR;
    strlcpy(current->cwd, c, sizeof current->cwd);
    return 0;
}

static int sys_getcwd(char *buf, uint32_t n) {
    uint32_t l = strlen(current->cwd) + 1;
    if (n < l) return -ERANGE;
    if (!uok(buf, l, true)) return -EFAULT;
    memcpy(buf, current->cwd, l);
    return l - 1;
}

static int sys_spawn(const char *up, char *const *uargv, char *const *uenvp, const struct spawn_opts *uo) {
    UPATH(p, up);
    struct spawn_opts o = { { -1, -1, -1 }, 0, 0 };
    char cwd[PATH_MAX];
    if (uo) {
        if (!uok(uo, sizeof *uo, false)) return -EFAULT;
        o = *uo;
        if (o.cwd) { int r = ustr(o.cwd, cwd, sizeof cwd); if (r < 0) return r; o.cwd = cwd; }
    }
    int err;
    char **argv = uvec(uargv, &err);
    if (err) return err;
    char **envp = uvec(uenvp, &err);
    if (err) { kfree(argv); return err; }
    char *defargv[] = { p, 0 };
    int r = proc_spawn(p, argv ? argv : defargv, envp, &o);
    kfree(argv); kfree(envp);
    return r;
}

static int sys_sbrk(int32_t inc) {
    uint32_t old = current->brk, nb = old + inc;
    if (inc > 0 && (nb < old || nb > USER_STACK_TOP - (8u << 20))) return -ENOMEM;
    if (inc < 0 && nb < current->brk_start) return -EINVAL;
    if (inc > 0 && vmm_alloc_range(current->dir, ALIGN_UP(old, PAGE_SIZE),
                                   ALIGN_UP(nb, PAGE_SIZE) - ALIGN_UP(old, PAGE_SIZE), PTE_U | PTE_W) < 0)
        return -ENOMEM;
    if (inc < 0) vmm_unmap_range(current->dir, ALIGN_UP(nb, PAGE_SIZE), ALIGN_UP(old, PAGE_SIZE) - ALIGN_UP(nb, PAGE_SIZE), true);
    current->brk = nb;
    return old;
}

static int sys_pipe(int *ufds) {
    if (!uok(ufds, 8, true)) return -EFAULT;
    struct file *r, *w;
    int e = pipe_create(&r, &w);
    if (e < 0) return e;
    int a = fd_alloc(r);
    if (a < 0) { vfs_close(r); vfs_close(w); return a; }
    int b = fd_alloc(w);
    if (b < 0) { current->fds[a] = 0; vfs_close(r); vfs_close(w); return b; }
    ufds[0] = a; ufds[1] = b;
    return 0;
}

static int sys_dup2(int old, int nfd) {
    struct file *f = fd_get(old);
    if (!f || nfd < 0 || nfd >= MAX_FDS) return -EBADF;
    if (old == nfd) return nfd;
    if (current->fds[nfd]) vfs_close(current->fds[nfd]);
    f->refs++;
    current->fds[nfd] = f;
    return nfd;
}

static int sys_dup(int fd) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    int n = fd_alloc(f);
    if (n >= 0) f->refs++;
    return n;
}

static int sys_ioctl(int fd, int req, void *arg) {
    struct file *f = fd_get(fd);
    if (!f) return -EBADF;
    if (!uok(arg, 16, true)) return -EFAULT;        /* every ioctl arg is small */
    if (req == FIONREAD && S_ISREG(f->vn->mode)) { *(int *)arg = f->vn->size - f->off; return 0; }
    return f->vn->ops->ioctl ? f->vn->ops->ioctl(f->vn, req, arg) : -ENOTTY;
}

static uint32_t count_user_pages(uint32_t dir) {
    uint32_t *d = (uint32_t *)dir, n = 0;
    for (uint32_t i = USER_BASE >> 22; i < USER_TOP >> 22; i++) {
        if (!(d[i] & PTE_P)) continue;
        uint32_t *t = (uint32_t *)(d[i] & ~0xFFFu);
        for (int j = 0; j < 1024; j++) n += t[j] & PTE_P;
    }
    return n;
}

static int sys_proclist(struct procinfo *buf, int max) {
    if (max < 0 || !uok(buf, max * sizeof *buf, true)) return -EFAULT;
    int n = 0;
    for (struct task *t = task_list(); t && n < max; t = t->next, n++) {
        struct procinfo *p = &buf[n];
        p->pid = t->pid; p->ppid = t->ppid; p->state = t->state; p->cpu_ms = t->cpu_ms;
        p->mem_kb = t->user && t->state != PS_ZOMBIE ? count_user_pages(t->dir) * 4 : 0;
        strlcpy(p->name, t->name, sizeof p->name);
    }
    return n;
}

static int sys_sysinfo(struct sysinfo *si) {
    if (!uok(si, sizeof *si, true)) return -EFAULT;
    si->uptime_ms = ticks_ms;
    si->mem_total_kb = pmm_total_count() * 4;
    si->mem_free_kb = pmm_free_count() * 4;
    si->heap_total_kb = heap_size() >> 10;
    si->heap_used_kb = heap_used() >> 10;
    si->nprocs = 0;
    for (struct task *t = task_list(); t; t = t->next) si->nprocs++;
    si->idle_ms = idle_ms();
    si->screen_w = boot_info->fb_width;
    si->screen_h = boot_info->fb_height;
    return 0;
}

static int sys_poll(struct pollfd *ufds, int n, int timeout) {
    if (n < 0 || n > 64 || !uok(ufds, n * sizeof *ufds, true)) return -EFAULT;
    uint64_t deadline = timeout > 0 ? ticks_ms + timeout : 0;
    uint32_t f = irq_save();
    for (;;) {
        int ready = 0;
        for (int i = 0; i < n; i++) {
            struct file *fl = fd_get(ufds[i].fd);
            ufds[i].revents = 0;
            if (!fl) { if (ufds[i].fd >= 0) { ufds[i].revents = POLLHUP; ready++; } continue; }
            int ev = fl->vn->ops->poll ? fl->vn->ops->poll(fl->vn, ufds[i].events)
                                       : (ufds[i].events & (POLLIN | POLLOUT));
            ufds[i].revents = ev;
            if (ev) ready++;
        }
        if (ready || timeout == 0) { irq_restore(f); return ready; }
        uint32_t left = 0;
        if (timeout > 0) {
            if (ticks_ms >= deadline) { irq_restore(f); return 0; }
            left = deadline - ticks_ms;
        }
        if (sleep_on(&poll_chan, left) < 0) { irq_restore(f); return -EINTR; }
    }
}

static int sys_beep(uint32_t hz, uint32_t ms) {
    if (hz < 20 || hz > 20000 || ms > 5000) return -EINVAL;
    speaker_tone(hz);
    sleep_ms(ms);
    speaker_tone(0);
    return 0;
}

static NORETURN void sys_shutdown(int reboot) {
    kprintf("shutdown: syncing disks\n");
    ext2_sync_all();
    cli();
    if (reboot) { while (inb(0x64) & 2) {} outb(0x64, 0xFE); }
    outw(0x604, 0x2000);           /* QEMU q35/piix ACPI power off */
    outw(0xB004, 0x2000);          /* older QEMU / Bochs */
    outb(0xF4, 0);                 /* isa-debug-exit */
    for (;;) hlt();
}

static int sys_klog(char *buf, uint32_t n) {
    if (!uok(buf, n, true)) return -EFAULT;
    return klog_read(buf, 0, n);
}

__attribute__((weak)) int win_syscall(int num, uint32_t a, uint32_t b, uint32_t c) { return -ENOSYS; }

static void syscall_handler(struct regs *r) {
    uint32_t a = r->ebx, b = r->ecx, c = r->edx, d = r->esi, e = r->edi;
    int ret;
    sti();                         /* syscalls run with interrupts on */
    switch (r->eax) {
    case SYS_exit:     proc_exit((int)a);
    case SYS_read:     ret = sys_rw(a, (void *)b, c, false); break;
    case SYS_write:    ret = sys_rw(a, (void *)b, c, true); break;
    case SYS_open:     ret = sys_open((const char *)a, b); break;
    case SYS_close:    ret = sys_close(a); break;
    case SYS_lseek:    { struct file *f = fd_get(a); ret = f ? vfs_seek(f, b, c) : -EBADF; break; }
    case SYS_stat:     ret = sys_stat((const char *)a, (struct stat *)b); break;
    case SYS_fstat:    ret = sys_fstat(a, (struct stat *)b); break;
    case SYS_readdir:  ret = sys_readdir(a, (struct dirent *)b); break;
    case SYS_mkdir: case SYS_unlink: case SYS_rmdir:
                       ret = sys_path1(r->eax, (const char *)a); break;
    case SYS_rename:   ret = sys_rename((const char *)a, (const char *)b); break;
    case SYS_chdir:    ret = sys_chdir((const char *)a); break;
    case SYS_getcwd:   ret = sys_getcwd((char *)a, b); break;
    case SYS_spawn:    ret = sys_spawn((const char *)a, (char *const *)b, (char *const *)c, (const struct spawn_opts *)d); break;
    case SYS_waitpid:
        if (b && !uok((void *)b, 4, true)) { ret = -EFAULT; break; }
        ret = proc_wait(a, (int *)b, c); break;
    case SYS_getpid:   ret = current->pid; break;
    case SYS_getppid:  ret = current->ppid; break;
    case SYS_sbrk:     ret = sys_sbrk(a); break;
    case SYS_sleep:    sleep_ms(a); ret = current->pending_sig ? -EINTR : 0; break;
    case SYS_uptime:   ret = (uint32_t)ticks_ms; break;
    case SYS_time:     ret = rtc_now(); break;
    case SYS_kill:     ret = proc_kill(a, b); break;
    case SYS_pipe:     ret = sys_pipe((int *)a); break;
    case SYS_dup2:     ret = sys_dup2(a, b); break;
    case SYS_dup:      ret = sys_dup(a); break;
    case SYS_ioctl:    ret = sys_ioctl(a, b, (void *)c); break;
    case SYS_proclist: ret = sys_proclist((struct procinfo *)a, b); break;
    case SYS_sysinfo:  ret = sys_sysinfo((struct sysinfo *)a); break;
    case SYS_yield:    yield(); ret = 0; break;
    case SYS_poll:     ret = sys_poll((struct pollfd *)a, b, c); break;
    case SYS_ftruncate: {
        struct file *f = fd_get(a);
        ret = !f ? -EBADF : f->vn->ops->truncate ? f->vn->ops->truncate(f->vn, b) : -EINVAL;
        break;
    }
    case SYS_win_create: case SYS_win_flip: case SYS_win_set:
        ret = win_syscall(r->eax, a, b, c); break;
    case SYS_beep:     ret = sys_beep(a, b); break;
    case SYS_shutdown: sys_shutdown(a);
    case SYS_klog:     ret = sys_klog((char *)a, b); break;
    case SYS_socket: case SYS_connect: case SYS_bind: case SYS_sendto: case SYS_recvfrom:
    case SYS_resolve: case SYS_netinfo:
                       ret = net_syscall(r->eax, a, b, c, d, e); break;
    default:           ret = -ENOSYS;
    }
    r->eax = ret;
    cli();
}

void syscall_init(void) { isr_register(0x80, syscall_handler); }
