/* fwcfg.c - QEMU fw_cfg (ports 0x510/0x511): `-fw_cfg name=opt/vibeos/autostart,file=F` hands the guest a host file at boot,
 * so tools/vmtest.py can start a command without typing it or editing the disk image. No QEMU: the signature check fails. */
#include "kernel.h"

static uint32_t be(int n) { uint32_t v = 0; while (n--) v = v << 8 | inb(0x511); return v; }

/* kmalloc'd, NUL-terminated copy of the named fw_cfg file, or 0 */
char *fwcfg_read(const char *name, uint32_t *size) {
    outw(0x510, 0);
    if (be(4) != 0x51454D55) return 0;                  /* "QEMU" */
    outw(0x510, 0x19);                                  /* file directory */
    for (uint32_t n = be(4); n--; ) {
        uint32_t sz = be(4), sel = be(2); char nm[56];
        be(2);
        for (int i = 0; i < 56; i++) nm[i] = inb(0x511);
        if (strncmp(nm, name, 56)) continue;
        char *b = kmalloc(sz + 1);
        if (!b) return 0;
        outw(0x510, sel);
        for (uint32_t i = 0; i < sz; i++) b[i] = inb(0x511);
        b[sz] = 0;
        if (size) *size = sz;
        return b;
    }
    return 0;
}
