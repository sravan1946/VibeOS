/* vibefetch : neofetch-style system summary with a truecolour diamond */
#include <vutil.h>
#include <syscall.h>
#include <sys/ioctl.h>

#define H 13                       /* logo rows; widest row is 13 units of 2 cells */
static int color;

static void rgb(int fg, int r, int g, int b) { if (color) printf("\x1b[%d;2;%d;%d;%dm", fg ? 38 : 48, r, g, b); }
static void reset(void) { if (color) fputs("\x1b[0m", stdout); }

static void logo_row(int i) {
    int d = i < H / 2 ? H / 2 - i : i - H / 2;
    int units = 1 + 2 * (H / 2 - d), pad = H - units;
    printf("%*s", pad, "");
    /* gradient cyan -> violet -> pink down the diamond; left facet lit, right facet shaded */
    int t = i * 255 / (H - 1);
    int r = t < 128 ? 70 + t * 110 / 128 : 180 + (t - 128) * 75 / 127;
    int g = t < 128 ? 210 - t * 110 / 128 : 100 - (t - 128) * 0 / 127;
    int b = t < 128 ? 255 : 255 - (t - 128) * 75 / 127;
    for (int j = 0; j < units; j++) {
        int f = j < units / 2 ? 100 : j == units / 2 ? 82 : 62;        /* percent brightness */
        rgb(1, r * f / 100, g * f / 100, b * f / 100);
        fputs("\xe2\x96\x88\xe2\x96\x88", stdout);                      /* two full blocks */
    }
    reset();
    printf("%*s", pad + 3, "");
}

static void key(const char *k) {
    if (color) fputs("\x1b[1;38;2;110;200;255m", stdout);
    printf("%s", k);
    reset();
    fputs(": ", stdout);
}

int main(void) {
    color = isatty(1);
    struct sysinfo si;
    sysinfo(&si);
    char cpu[96] = "unknown", rel[32] = "2.0", ver[64] = "";
    int fd = open("/proc/cpuinfo", O_RDONLY);
    if (fd >= 0) {
        char b[256];
        int n = read(fd, b, sizeof b - 1);
        close(fd);
        if (n > 0) {
            b[n] = 0;
            char *p = strstr(b, "model name:");
            if (p) {
                p += 11;
                while (*p == ' ') p++;
                char *e = strchr(p, '\n');
                if (e) *e = 0;
                strlcpy(cpu, p, sizeof cpu);
            }
        }
    }
    fd = open("/proc/version", O_RDONLY);
    if (fd >= 0) {
        char b[128];
        int n = read(fd, b, sizeof b - 1);
        close(fd);
        if (n > 0) {
            b[n] = 0;
            char *e = strchr(b, '\n');
            if (e) *e = 0;
            char *sp = strchr(b, ' ');
            if (sp) sscanf(sp + 1, "%31s", rel);
            char *bu = strstr(b, "built ");
            if (bu) strlcpy(ver, bu + 6, sizeof ver);
        }
    }
    struct winsize ws = { 0 };
    ioctl(1, TIOCGWINSZ, &ws);
    uint32_t s = si.uptime_ms / 1000;
    char up[48];
    if (s >= 86400) snprintf(up, sizeof up, "%ud %uh %um", s / 86400, s % 86400 / 3600, s % 3600 / 60);
    else if (s >= 3600) snprintf(up, sizeof up, "%uh %um", s / 3600, s % 3600 / 60);
    else snprintf(up, sizeof up, "%um %us", s / 60, s % 60);
    const char *sh = getenv("SHELL"), *term = getenv("TERM"), *user = getenv("USER");
    uint32_t used = si.mem_total_kb - si.mem_free_kb;

    char line[H + 2][160];
    int n = 0;
    #define L(...) snprintf(line[n++], sizeof line[0], __VA_ARGS__)
    char hdr[64];
    snprintf(hdr, sizeof hdr, "%s@vibeos", user && *user ? user : "root");
    L("%s", hdr);
    L("%.*s", (int)strlen(hdr), "------------------------------------------------");
    L("OS|VibeOS 2.0 i686");
    L("Kernel|%s%s%s%s", "vibe-", rel, ver[0] ? " built " : "", ver);
    L("Uptime|%s", up);
    L("CPU|%s", cpu);
    L("Memory|%u MiB / %u MiB (%u%%)", used / 1024, si.mem_total_kb / 1024, si.mem_total_kb ? used * 100 / si.mem_total_kb : 0);
    L("Display|%ux%u", si.screen_w, si.screen_h);
    L("Shell|%s", sh && *sh ? sh : "/bin/sh");
    L("Terminal|%s %ux%u", term && *term ? term : "vibeterm", ws.ws_col, ws.ws_row);

    for (int i = 0; i < H; i++) {
        logo_row(i);
        int row = i - 1;                               /* info starts one row below the top */
        if (row >= 0 && row < n) {
            char *bar = strchr(line[row], '|');
            if (bar) { *bar = 0; key(line[row]); fputs(bar + 1, stdout); }
            else if (row == 0) { if (color) fputs("\x1b[1;38;2;230;130;255m", stdout); fputs(line[row], stdout); reset(); }
            else fputs(line[row], stdout);
        } else if (row == n + 1) {                     /* palette: 8 normal + 8 bright */
            for (int c = 0; c < 16; c++) {
                if (color) printf("\x1b[%dm  \x1b[0m", (c < 8 ? 40 + c : 100 + c - 8));
                else fputs("  ", stdout);
            }
        }
        putchar('\n');
    }
    return 0;
}
