/* mouse.c - PS/2 mouse (IntelliMouse wheel) + VMware/QEMU vmmouse absolute
 * positioning, so the pointer tracks the host cursor without a grab. */
#include "arch.h"
#include "mouse.h"

void (*mouse_sink)(int x, int y, int buttons, int dz);
static int mx, my, scr_w, scr_h, wheel, absolute;
static uint8_t pkt[4];
static int idx;

static void wait_in(void)  { for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) {} }
static void wait_out(void) { for (int i = 0; i < 100000 && !(inb(0x64) & 1); i++) {} }
static void aux_write(uint8_t v) { wait_in(); outb(0x64, 0xD4); wait_in(); outb(0x60, v); wait_out(); inb(0x60); }

/* ---- vmmouse backdoor ---- */
#define VMW_MAGIC 0x564D5868
#define VMW_PORT  0x5658
struct vmw { uint32_t a, b, c, d; };
static void vmw_cmd(struct vmw *r, uint32_t cmd, uint32_t arg) {
    __asm__ volatile("inl %%dx, %%eax"
                     : "=a"(r->a), "=b"(r->b), "=c"(r->c), "=d"(r->d)
                     : "a"(VMW_MAGIC), "b"(arg), "c"(cmd), "d"(VMW_PORT));
}

static bool vmmouse_init(void) {
    struct vmw r;
    vmw_cmd(&r, 10, ~0u);                       /* GETVERSION */
    if (r.b != VMW_MAGIC) return false;
    vmw_cmd(&r, 41, 0x45414552);                /* ABSPOINTER_COMMAND: READ_ID */
    vmw_cmd(&r, 40, 0);                         /* STATUS */
    if ((r.a & 0xFFFF) == 0) return false;
    vmw_cmd(&r, 39, 1);                         /* DATA: id */
    if (r.a != 0x3442554A) return false;
    vmw_cmd(&r, 41, 0x53424152);                /* REQUEST_ABSOLUTE */
    return true;
}

static void vmmouse_poll(void) {
    struct vmw r;
    for (;;) {
        vmw_cmd(&r, 40, 0);
        if (r.a == 0xFFFF0000) { vmmouse_init(); return; }   /* error: reset */
        uint32_t n = r.a & 0xFFFF;
        if (n < 4) return;
        vmw_cmd(&r, 39, 4);
        int b = 0;
        if (r.a & 0x20) b |= 1;
        if (r.a & 0x10) b |= 2;
        if (r.a & 0x08) b |= 4;
        mx = (int)(r.b * (uint32_t)scr_w / 65536);
        my = (int)(r.c * (uint32_t)scr_h / 65536);
        int dz = (int8_t)(r.d & 0xFF);
        if (mouse_sink) mouse_sink(mx, my, b, dz);
    }
}

static void mouse_irq(struct regs *r) {
    (void)r;
    uint8_t st = inb(0x64);
    if (!(st & 0x20)) return;                   /* not aux data */
    uint8_t v = inb(0x60);
    if (idx == 0 && !(v & 8)) return;           /* resync */
    pkt[idx++] = v;
    if (idx < (wheel ? 4 : 3)) return;
    idx = 0;
    int dx = pkt[1] - ((pkt[0] << 4) & 0x100);
    int dy = pkt[2] - ((pkt[0] << 3) & 0x100);
    int dz = wheel ? (int8_t)(pkt[3] << 4) >> 4 : 0;
    /* with vmmouse the PS/2 packet is only a doorbell: QEMU fakes it with dx=1 and the
     * PS/2 button state, which stays 0 because buttons go to the vmmouse queue. Feeding it
     * on sent a release after every move, so drags (and touchpad tap-drags) never held. */
    if (absolute) { vmmouse_poll(); return; }
    int b = pkt[0] & 7;
    mx = MAX(0, MIN(scr_w - 1, mx + dx));
    my = MAX(0, MIN(scr_h - 1, my - dy));
    if (mouse_sink) mouse_sink(mx, my, b, -dz);
}

void mouse_init(int w, int h) {
    scr_w = w; scr_h = h; mx = w / 2; my = h / 2;
    wait_in(); outb(0x64, 0xA8);                /* enable aux */
    wait_in(); outb(0x64, 0x20); wait_out();
    uint8_t cfg = inb(0x60) | 2;                /* IRQ12 on */
    cfg &= ~0x20;
    wait_in(); outb(0x64, 0x60); wait_in(); outb(0x60, cfg);
    aux_write(0xF6);                            /* defaults */
    aux_write(0xF3); aux_write(200);            /* IntelliMouse knock */
    aux_write(0xF3); aux_write(100);
    aux_write(0xF3); aux_write(80);
    aux_write(0xF2); wait_out();
    wheel = inb(0x60) == 3;
    aux_write(0xF4);                            /* enable streaming */
    absolute = vmmouse_init();
    kprintf("mouse: ps/2%s%s\n", wheel ? " + wheel" : "", absolute ? ", vmmouse absolute" : "");
    irq_register(12, mouse_irq);
}

void mouse_pos(int *x, int *y) { *x = mx; *y = my; }

void mouse_set_screen(int w, int h) {
    uint32_t f = irq_save();
    scr_w = w; scr_h = h;
    mx = MIN(mx, w - 1); my = MIN(my, h - 1);
    irq_restore(f);
}
