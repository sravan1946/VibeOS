/* testcmd.c - runtime command channel for tools/vmtest.py: lines on COM1 (UART RX interrupt, IRQ4), only when the host set the
 * fw_cfg flag opt/vibeos/testmode; without it serial input is ignored as ever. One line = "VERB ID TEXT", TEXT with \\ \n \t escapes:
 *   run ID CMD    /bin/sh -c CMD in the background, output to the kernel log; serial gets "[run ID] exit CODE" when it's done
 *   term ID CMD   a visible terminal window running CMD (kept open); "[term ID] started"
 *   type ID TEXT  TEXT as key presses into the focused window, in one go; "[type ID] ok"
 * "[testcmd] ready" says the channel is up. The exit line is echoed by the shell's own EXIT trap (a kthread parent's children are
 * auto-reaped, so the kernel can't wait for them): a command that kills the shell with a signal prints no line. */
#include "wm.h"

#define COM1 0x3F8
#define RXN 8192
#define LINEN 4096

char *fwcfg_read(const char *name, uint32_t *size);
int wm_type(const char *s, int n);

static char rx[RXN];
static volatile uint32_t rh, rt;

static void rx_irq(struct regs *r) {
    while (inb(COM1 + 5) & 1) {
        char c = inb(COM1);
        if (rh - rt < RXN) rx[rh++ % RXN] = c;
    }
    wakeup(rx);
}

static int unescape(char *s) {                          /* in place; returns the new length */
    char *o = s, *p = s;
    while (*p) {
        if (*p == '\\' && p[1]) { p++; *o++ = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p; p++; }
        else *o++ = *p++;
    }
    *o = 0;
    return o - s;
}

static void do_run(const char *id, const char *cmd) {
    char *sc = kmalloc(strlen(cmd) + 64);
    struct file *k = 0;
    vfs_open("/", "/dev/kmsg", O_RDWR, &k);
    if (!sc) { kprintf("[run %s] exit 127\n", id); return; }
    snprintf(sc, strlen(cmd) + 64, "trap 'echo \"[run %s] exit $?\" >/dev/kmsg' EXIT\n%s", id, cmd);
    char *argv[] = { "/bin/sh", "-c", sc, 0 }, *envp[] = { "PATH=/bin", "HOME=/home", 0 };
    struct file *fds[3] = { k, k, k };
    int pid = proc_spawn_ex("/bin/sh", argv, envp, "/home", fds, 0);
    if (pid < 0) { ext2_flush_dirty(); kprintf("[run %s] exit 127 (can't start sh: %d)\n", id, pid); }
    if (k) vfs_close(k);
    kfree(sc);
}

/* every ack goes out with the disk current (ext2 writes are delayed): the host may stop the VM the moment it reads it. The run exit
 * line comes from the shell through /dev/kmsg, which flushes first */
static void line(char *l) {
    char *id = strchr(l, ' '), *txt = "";
    if (!id) return;
    *id++ = 0;
    if ((txt = strchr(id, ' '))) *txt++ = 0; else txt = "";
    int n = unescape(txt);
    if (!strcmp(l, "run")) do_run(id, txt);
    else if (!strcmp(l, "term")) {
        char *argv[] = { "/bin/sh", "-c", txt, 0 };
        bool ok = termwin_open_argv(argv, true);
        ext2_flush_dirty();
        kprintf(ok ? "[term %s] started\n" : "[term %s] failed\n", id);
    } else if (!strcmp(l, "type")) {
        bool ok = wm_type(txt, n) >= 0;
        ext2_flush_dirty();
        kprintf(ok ? "[type %s] ok\n" : "[type %s] no focused window\n", id);
    }
    else kprintf("[testcmd] unknown verb '%s'\n", l);
}

static int testcmd(void *arg) {
    static char ln[LINEN];
    int n = 0;
    kprintf("[testcmd] ready\n");
    for (;;) {
        uint32_t f = irq_save();
        while (rh == rt) sleep_on(rx, 0);
        char c = rx[rt++ % RXN];
        irq_restore(f);
        if (c == '\r') continue;
        if (c != '\n') { if (n < LINEN - 1) ln[n++] = c; continue; }
        ln[n] = 0; n = 0;
        line(ln);
    }
    return 0;
}

void testcmd_init(void) {
    char *f = fwcfg_read("opt/vibeos/testmode", 0);
    if (!f) return;
    kfree(f);
    irq_register(4, rx_irq);
    outb(COM1 + 1, 0x01);                               /* IER: receive data available */
    kthread_create("testcmd", testcmd, 0);
}
