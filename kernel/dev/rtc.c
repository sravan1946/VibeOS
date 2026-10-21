/* rtc.c - CMOS real-time clock, read once at boot */
#include "kernel.h"

static uint32_t boot_epoch;

static uint8_t cmos(uint8_t reg) { outb(0x70, reg); return inb(0x71); }

static uint32_t days_from_civil(int y, int m, int d) {          /* Howard Hinnant's algorithm */
    y -= m <= 2;
    int era = y / 400, yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void rtc_init(void) {
    while (cmos(0x0A) & 0x80) {}                                  /* update in progress */
    uint8_t s = cmos(0), mi = cmos(2), h = cmos(4), d = cmos(7), mo = cmos(8), y = cmos(9), b = cmos(0x0B);
    if (!(b & 4)) {                                               /* BCD */
#define BCD(x) (((x) & 15) + ((x) >> 4) * 10)
        s = BCD(s); mi = BCD(mi); h = BCD(h & 0x7F) | (h & 0x80); d = BCD(d); mo = BCD(mo); y = BCD(y);
    }
    if (!(b & 2) && (h & 0x80)) h = ((h & 0x7F) + 12) % 24;        /* 12h clock */
    boot_epoch = days_from_civil(2000 + y, mo, d) * 86400u + h * 3600u + mi * 60u + s;
    kprintf("rtc: 20%02u-%02u-%02u %02u:%02u:%02u UTC\n", y, mo, d, h, mi, s);
}

uint32_t rtc_now(void) { return boot_epoch + (uint32_t)(ticks_ms / 1000); }
uint32_t ext2_now(void) { return rtc_now(); }
