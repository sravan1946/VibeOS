/* display.c - runtime mode setting on the Bochs/QEMU display adapter (PCI 1234:1111) through
 * its DISPI registers, and /dev/display: read lists the modes ("WxH", the current one marked
 * " *"), write "WxH" switches. The bootloader's VBE mode is what we start in; /etc/display
 * (same "WxH" text) is applied once the root filesystem is up. */
#include "console.h"
#include "arch.h"
#include "mouse.h"
#include "net.h"
#include "vfs.h"
#include "wm.h"

#define DISPI_IDX 0x1CE
#define DISPI_DAT 0x1CF
enum { D_ID, D_XRES, D_YRES, D_BPP, D_ENABLE, D_BANK, D_VIRT_W, D_VIRT_H, D_XOFF, D_YOFF, D_VRAM64K };
#define D_ENABLED 0x01
#define D_LFB     0x40

/* window buffers are mapped into apps in 16 MiB slots (wm.c), so that's the ceiling too */
#define MAX_BYTES (16u * 1024 * 1024 - 4096)

static uint32_t lfb, vram;             /* lfb = 0: no Bochs adapter, the boot mode is all there is */

static const struct { uint16_t w, h; } modes[] = {
    { 1024, 768 }, { 1152, 864 }, { 1280, 720 }, { 1280, 800 }, { 1280, 960 }, { 1280, 1024 },
    { 1366, 768 }, { 1440, 900 }, { 1600, 900 }, { 1600, 1200 }, { 1680, 1050 }, { 1920, 1080 },
    { 1920, 1200 }, { 2560, 1440 }, { 2560, 1600 },
};

static uint16_t rd(int i) { outw(DISPI_IDX, i); return inw(DISPI_DAT); }
static void wr(int i, uint16_t v) { outw(DISPI_IDX, i); outw(DISPI_DAT, v); }

static bool mode_ok(int w, int h) {
    return w >= 800 && h >= 600 && w <= 4096 && h <= 4096 && (uint32_t)ALIGN_UP(w, 8) * h * 4 <= MIN(vram, MAX_BYTES);
}

static void program(int w, int h) {
    wr(D_ENABLE, 0);
    wr(D_XRES, w); wr(D_YRES, h); wr(D_BPP, 32);
    wr(D_VIRT_W, w); wr(D_XOFF, 0); wr(D_YOFF, 0);
    wr(D_ENABLE, D_ENABLED | D_LFB);
}

/* program the adapter and repoint the framebuffer canvas + pointer scale; the compositor
 * (wm_set_mode) wraps this with its own buffers. */
int display_mode(int w, int h) {
    if (!lfb) return w == fb.w && h == fb.h ? 0 : -ENODEV;
    if (!mode_ok(w, h)) return -EINVAL;
    /* QEMU rounds XRES down to a multiple of 8, so 1366 is programmed as 1368 and the
     * compositor leaves the last two columns black */
    int hw = ALIGN_UP(w, 8), ow = boot_info->fb_pitch / 4, oh = fb.h;
    program(hw, h);
    if (rd(D_XRES) != hw || rd(D_YRES) != h || rd(D_BPP) != 32) {   /* the device refused it */
        program(ow, oh);
        return -EINVAL;
    }
    uint32_t pitch = rd(D_VIRT_W) * 4;
    boot_info->fb_width = w; boot_info->fb_height = h; boot_info->fb_pitch = pitch;   /* = the current mode now */
    fb_set_mode((uint32_t *)lfb, w, h, pitch / 4);
    mouse_set_screen(w, h);
    kprintf("display: %dx%d\n", w, h);
    return 0;
}

static int set_mode(int w, int h) { return wm_running() ? wm_set_mode(w, h) : display_mode(w, h); }

static bool parse(const char *s, int n, int *w, int *h) {
    int v[2] = { 0, 0 }, k = 0, i = 0;
    for (; i < n && k < 2; i++) {
        if (s[i] >= '0' && s[i] <= '9') v[k] = v[k] * 10 + (s[i] - '0');
        else if ((s[i] == 'x' || s[i] == 'X') && k == 0) k = 1;
        else break;
        if (v[k] > 100000) return false;
    }
    *w = v[0]; *h = v[1];
    return k == 1 && v[0] && v[1];
}

static int gen(char *b, int n) {
    int o = 0;
    bool cur_listed = false;
    for (unsigned i = 0; i < ARRAY_LEN(modes); i++) {
        if (lfb ? !mode_ok(modes[i].w, modes[i].h) : modes[i].w != fb.w || modes[i].h != fb.h) continue;
        bool cur = modes[i].w == fb.w && modes[i].h == fb.h;
        cur_listed |= cur;
        o += snprintf(b + o, n - o, "%ux%u%s\n", modes[i].w, modes[i].h, cur ? " *" : "");
    }
    if (!cur_listed) o += snprintf(b + o, n - o, "%dx%d *\n", fb.w, fb.h);
    return o;
}

static int disp_read(struct vnode *v, uint32_t off, void *buf, uint32_t n) {
    char tmp[512];
    int len = gen(tmp, sizeof tmp);
    if (off >= (uint32_t)len) return 0;
    n = MIN(n, (uint32_t)len - off);
    memcpy(buf, tmp + off, n);
    return n;
}

static int disp_write(struct vnode *v, uint32_t off, const void *buf, uint32_t n) {
    int w, h;
    if (!parse(buf, n, &w, &h)) return -EINVAL;
    int r = set_mode(w, h);
    return r < 0 ? r : (int)n;
}

static int disp_truncate(struct vnode *v, uint32_t len) { return 0; }
static int disp_poll(struct vnode *v, int ev) { return ev & (POLLIN | POLLOUT); }
const struct vnode_ops display_ops = { .read = disp_read, .write = disp_write, .truncate = disp_truncate, .poll = disp_poll };

/* after pci_init and the root mount: find the adapter, map all of VRAM write-combining, apply /etc/display */
void display_init(void) {
    struct pci_dev d;
    uint16_t id = rd(D_ID);
    if (pci_find(0x1234, 0x1111, &d) < 0 || id < 0xB0C0 || id > 0xB0CF) {
        kprintf("display: no Bochs adapter, staying at %dx%d\n", fb.w, fb.h);
        return;
    }
    lfb = d.bar[0] & ~0xFu;
    vram = (uint32_t)rd(D_VRAM64K) << 16;
    if (!vram) vram = 16u << 20;
    vmm_map_wc(lfb, vram);
    kprintf("display: bochs dispi %x, %u MiB vram at %x\n", id, vram >> 20, lfb);
    void *data; uint32_t size;
    if (vfs_read_all("/etc/display", &data, &size) < 0) return;
    int w, h;
    if (parse(data, size, &w, &h) && (w != fb.w || h != fb.h) && set_mode(w, h) < 0)
        kprintf("\x1b[31mdisplay: can't set %dx%d from /etc/display\x1b[0m\n", w, h);
    kfree(data);
}
