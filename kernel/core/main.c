#include "proc.h"
#include "console.h"
#include "tty.h"
#include "wm.h"
#include "mouse.h"
#include "net.h"

struct boot_info *boot_info;
void rtc_init(void);
void devfs_init(void);
void devfs_audio(void);
NORETURN void sched_idle(void);

/* storage comes from dev/ata.c + fs/ext2.c */
__attribute__((weak)) int ata_init(void) { return 0; }
__attribute__((weak)) struct blockdev *blockdev_root_partition(void) { return 0; }
__attribute__((weak)) int ext2_mount(struct blockdev *d, struct vnode **root) { return -ENODEV; }
__attribute__((weak)) int ext2_sync_all(void) { return 0; }

static char brand[49];
const char *cpu_brand(void) { return brand; }

static void cpu_detect(void) {
    uint32_t r[12], max;
    __asm__ volatile("cpuid" : "=a"(max) : "a"(0x80000000) : "ebx", "ecx", "edx");
    if (max < 0x80000004) { strcpy(brand, "unknown i686"); return; }
    for (int i = 0; i < 3; i++)
        __asm__ volatile("cpuid" : "=a"(r[i * 4]), "=b"(r[i * 4 + 1]), "=c"(r[i * 4 + 2]), "=d"(r[i * 4 + 3])
                         : "a"(0x80000002 + i));
    memcpy(brand, r, 48);
    char *p = brand;
    while (*p == ' ') p++;
    memmove(brand, p, strlen(p) + 1);
}

/* the boot console: a tty whose output is the boot text console */
static struct tty *console;
static void console_out(void *ctx, const char *s, int n) {
    char buf[257];
    while (n > 0) {
        int k = MIN(n, 256);
        memcpy(buf, s, k); buf[k] = 0;
        for (int i = 0; i < k; i++) if (buf[i] == '\r') buf[i] = ' ';
        bootcon_write(buf);
        s += k; n -= k;
    }
}
static void console_key(const struct gui_event *ev) { tty_key(console, ev); }

static void mount_root(void) {
    int disks = ata_init();
    struct blockdev *part = blockdev_root_partition();
    struct vnode *root;
    if (!disks || !part) { kprintf("\x1b[31mno root partition found\x1b[0m\n"); return; }
    int r = ext2_mount(part, &root);
    if (r < 0) { kprintf("\x1b[31mext2 mount failed: %d\x1b[0m\n", r); return; }
    vfs_mount("/", root);
    kprintf("root: ext2 on %s (%u MiB)\n", part->name, part->sectors / 2048);
}

static int start_init(void *arg) {
    char *argv[] = { "/bin/init", 0 };
    char *envp[] = { "PATH=/bin", "HOME=/home", "TERM=xterm-256color", 0 };
    struct file *f;
    for (int i = 0; i < 3; i++)
        if ((f = vfs_open_vnode(&console->vn, O_RDWR))) current->fds[i] = f;
    current->tty = console;
    int pid = proc_spawn("/bin/init", argv, envp, 0);
    if (pid < 0) {
        argv[0] = "/bin/sh";
        pid = proc_spawn("/bin/sh", argv, envp, 0);
    }
    if (pid < 0) { kprintf("\x1b[31mcan't start /bin/init or /bin/sh: %d\x1b[0m\n", pid); return 1; }
    for (;;) {
        int st, p = proc_wait(-1, &st, 0);
        if (p == pid) {
            kprintf("init exited (%d), restarting shell\n", st);
            argv[0] = "/bin/sh";
            pid = proc_spawn("/bin/sh", argv, envp, 0);
            if (pid < 0) return 1;
        }
    }
}

void kmain(struct boot_info *bi) {
    boot_info = bi;
    serial_init();
    if (bi->magic != 0x45424956) { kprintf("bad boot_info magic\n"); for (;;) hlt(); }
    fb_init();
    cpu_detect();
    kprintf("\x1b[35mVibeOS\x1b[0m 2.0 on %s, %ux%u\n", brand, bi->fb_width, bi->fb_height);

    splash_progress(5);
    gdt_init();
    idt_init();
    pmm_init();
    uint32_t ram_mb = pmm_total_count() / 256;
    uint32_t heap_bytes = (ram_mb >= 200 ? 32 : 16) << 20;
    uint32_t heap = pmm_alloc_contig(heap_bytes / PAGE_SIZE);
    if (!heap) panic("no memory for kernel heap");
    heap_init(heap, heap_bytes);
    vmm_init();
    vmm_map_mmio(bi->fb_addr, bi->fb_pitch * bi->fb_height);
    kprintf("memory: %u MiB usable, %u MiB kernel heap\n", ram_mb, heap_bytes >> 20);

    splash_progress(15);
    pit_init();
    rtc_init();
    sched_init();
    syscall_init();
    keyboard_init();
    console = tty_create(console_out, 0, 128, 36);
    key_sink = console_key;
    sti();
    splash_progress(30);

    devfs_init();
    mount_root();
    net_init();
    if (ac97_init() == 0) devfs_audio();
    splash_progress(60);
    mouse_init(bi->fb_width, bi->fb_height);
    splash_progress(75);
    wm_init();
    (void)start_init;
    sched_idle();
}
