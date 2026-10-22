/* devfs.c - /dev (null zero random tty kmsg clipboard screen display) and /proc (text files) */
#include "proc.h"
#include "tty.h"

struct snode { const char *name; struct vnode vn; };
extern size_t klog_read(char *buf, size_t off, size_t n);
extern size_t klog_size(void);
extern const char *cpu_brand(void);
extern int wm_screen_read(uint32_t off, void *buf, uint32_t n);
extern void wm_notify(const char *msg, int n);
extern const struct vnode_ops display_ops;

/* ---- tiny static directory helper ---- */
struct sdir { struct vnode vn; struct snode *nodes; int n; };

static int sdir_readdir(struct vnode *d, uint32_t *cookie, struct dirent *out) {
    struct sdir *sd = d->priv;
    if (*cookie >= (uint32_t)sd->n) return 0;
    struct snode *s = &sd->nodes[(*cookie)++];
    out->d_ino = s->vn.ino;
    out->d_type = S_ISDIR(s->vn.mode) ? DT_DIR : S_ISCHR(s->vn.mode) ? DT_CHR : DT_REG;
    strlcpy(out->d_name, s->name, sizeof out->d_name);
    return 1;
}

static int sdir_lookup(struct vnode *d, const char *name, struct vnode **out) {
    struct sdir *sd = d->priv;
    for (int i = 0; i < sd->n; i++)
        if (!strcmp(sd->nodes[i].name, name)) {
            struct vnode *v = &sd->nodes[i].vn;
            if (!strcmp(name, "tty") && d->ino == 1) {        /* /dev/tty = caller's terminal */
                if (!current->proc->tty) return -ENXIO;
                v = &current->proc->tty->vn;
            }
            if (v->ops->clone) return v->ops->clone(v, out);
            vnode_get(v);
            *out = v;
            return 0;
        }
    return -ENOENT;
}

static const struct vnode_ops sdir_ops = { .readdir = sdir_readdir, .lookup = sdir_lookup };

/* ---- /dev nodes ---- */
static uint32_t rng_state = 0x2545F491;
static uint32_t rng(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    rng_state ^= lo; rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5;
    return rng_state;
}

static int null_read(struct vnode *v, uint32_t off, void *b, uint32_t n) { return 0; }
static int sink_write(struct vnode *v, uint32_t off, const void *b, uint32_t n) { return n; }
static int zero_read(struct vnode *v, uint32_t off, void *b, uint32_t n) { memset(b, 0, n); return n; }
static int random_read(struct vnode *v, uint32_t off, void *b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) ((uint8_t *)b)[i] = rng() >> 11;
    return n;
}
static int kmsg_read(struct vnode *v, uint32_t off, void *b, uint32_t n) { return klog_read(b, off, n); }
static int screen_read(struct vnode *v, uint32_t off, void *b, uint32_t n) { return wm_screen_read(off, b, n); }
static int notify_write(struct vnode *v, uint32_t off, const void *b, uint32_t n) { wm_notify(b, n); return n; }
static int always_ready(struct vnode *v, int ev) { return ev & (POLLIN | POLLOUT); }

static const struct vnode_ops null_ops = { .read = null_read, .write = sink_write, .poll = always_ready };
static const struct vnode_ops zero_ops = { .read = zero_read, .write = sink_write, .poll = always_ready };
static const struct vnode_ops random_ops = { .read = random_read, .write = sink_write, .poll = always_ready };
static const struct vnode_ops kmsg_ops = { .read = kmsg_read, .poll = always_ready };
static const struct vnode_ops screen_ops = { .read = screen_read, .poll = always_ready };
static const struct vnode_ops notify_ops = { .write = notify_write, .poll = always_ready };
static const struct vnode_ops tty_placeholder_ops = { 0 };

/* /dev/clipboard: one shared buffer, a regular file so offsets advance; O_TRUNC + write replaces it */
#define CLIP_MAX (1u << 20)
static char *clip;
static uint32_t clip_cap;
static struct vnode *clip_vn;
static int clip_read(struct vnode *v, uint32_t off, void *b, uint32_t n) {
    if (off >= v->size) return 0;
    n = MIN(n, v->size - off);
    memcpy(b, clip + off, n);
    return n;
}
static int clip_write(struct vnode *v, uint32_t off, const void *b, uint32_t n) {
    if (off > v->size || n > CLIP_MAX - off) return -EFBIG;
    if (off + n > clip_cap) {
        uint32_t cap = MAX(off + n, clip_cap * 2);
        char *p = krealloc(clip, cap);
        if (!p) return -ENOMEM;
        clip = p; clip_cap = cap;
    }
    memcpy(clip + off, b, n);
    v->size = MAX(v->size, off + n);
    v->mtime = rtc_now();
    return n;
}
static int clip_truncate(struct vnode *v, uint32_t len) { if (len < v->size) v->size = len; return 0; }
static const struct vnode_ops clip_ops = { .read = clip_read, .write = clip_write, .truncate = clip_truncate, .poll = always_ready };

int clipboard_get(char *buf, int n) { return clip_read(clip_vn, 0, buf, n); }
void clipboard_set(const char *s, int n) { clip_vn->size = 0; clip_write(clip_vn, 0, s, n); }

#define CHR(nm, o, i) { nm, { .mode = S_IFCHR | 0666, .ino = i, .refs = 1, .ops = &o } }
static struct snode dev_nodes[] = {
    CHR("null", null_ops, 2), CHR("zero", zero_ops, 3), CHR("random", random_ops, 4),
    CHR("urandom", random_ops, 5), CHR("tty", tty_placeholder_ops, 6), CHR("kmsg", kmsg_ops, 7),
    { "screen", { .mode = S_IFREG | 0444, .ino = 11, .refs = 1, .ops = &screen_ops } },   /* REG so the offset advances */
    { "clipboard", { .mode = S_IFREG | 0666, .ino = 10, .refs = 1, .ops = &clip_ops } },
    CHR("notify", notify_ops, 12),
    { "display", { .mode = S_IFREG | 0666, .ino = 13, .refs = 1, .ops = &display_ops } },
    CHR("dsp", ac97_dsp_ops, 8), CHR("mixer", ac97_ops, 9),      /* listed only once ac97_init succeeds */
};
static struct sdir devdir = { { .mode = S_IFDIR | 0755, .ino = 1, .refs = 1, .ops = &sdir_ops }, dev_nodes, ARRAY_LEN(dev_nodes) - 2 };

/* ---- /proc: files generated on every read ---- */
static int gen_meminfo(char *b, int n) {
    return snprintf(b, n, "MemTotal: %u kB\nMemFree: %u kB\nKernelHeap: %u kB\nKernelHeapUsed: %u kB\n",
                    pmm_total_count() * 4, pmm_free_count() * 4, (unsigned)(heap_size() >> 10), (unsigned)(heap_used() >> 10));
}
static int gen_uptime(char *b, int n) {
    uint32_t ms = ticks_ms, idle = idle_ms();
    return snprintf(b, n, "%u.%02u %u.%02u\n", ms / 1000, ms % 1000 / 10, idle / 1000, idle % 1000 / 10);
}
static int gen_cpuinfo(char *b, int n) {
    return snprintf(b, n, "model name: %s\nscreen: %ux%u\n", cpu_brand(), boot_info->fb_width, boot_info->fb_height);
}
static int gen_version(char *b, int n) {
    return snprintf(b, n, "VibeOS 2.0 (i686) built " __DATE__ " " __TIME__ "\n");
}
static int gen_procs(char *b, int n) {
    static const char *st[] = { "run", "ready", "sleep", "zombie" };
    int o = snprintf(b, n, "%5s %5s %-7s %8s %s\n", "PID", "PPID", "STATE", "CPU(ms)", "NAME");
    for (struct task *t = task_list(); t && o < n; t = t->next)
        o += snprintf(b + o, n - o, "%5d %5d %-7s %8u %s\n", t->pid, t->ppid, st[t->state], t->cpu_ms, t->name);
    return o;
}

static int gen_read(struct vnode *v, uint32_t off, void *buf, uint32_t n) {
    int (*gen)(char *, int) = v->fs;
    char *tmp = kmalloc(8192);
    if (!tmp) return -ENOMEM;
    int len = gen(tmp, 8192);
    if (len > 8191) len = 8191;
    int r = off >= (uint32_t)len ? 0 : MIN(n, (uint32_t)len - off);
    memcpy(buf, tmp + off, r);
    kfree(tmp);
    return r;
}
static const struct vnode_ops gen_ops = { .read = gen_read };

#define GEN(nm, fn, i) { nm, { .mode = S_IFREG | 0444, .ino = i, .refs = 1, .ops = &gen_ops, .fs = fn, .size = 0 } }
static struct snode proc_nodes[] = {
    GEN("meminfo", gen_meminfo, 102), GEN("uptime", gen_uptime, 103), GEN("cpuinfo", gen_cpuinfo, 104),
    GEN("version", gen_version, 105), GEN("processes", gen_procs, 106),
};
static struct sdir procdir = { { .mode = S_IFDIR | 0555, .ino = 101, .refs = 1, .ops = &sdir_ops }, proc_nodes, ARRAY_LEN(proc_nodes) };

void devfs_audio(void) { devdir.n = ARRAY_LEN(dev_nodes); }

void devfs_init(void) {
    devdir.vn.priv = &devdir;
    for (int i = 0; i < (int)ARRAY_LEN(dev_nodes); i++)
        if (dev_nodes[i].vn.ops == &clip_ops) clip_vn = &dev_nodes[i].vn;
    procdir.vn.priv = &procdir;
    vfs_mount("/dev", &devdir.vn);
    vfs_mount("/proc", &procdir.vn);
}
