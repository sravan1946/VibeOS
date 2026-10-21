/* uname [-a] [-s] [-n] [-r] [-v] [-m]   (release/version parsed from /proc/version) */
#include <vutil.h>

int main(int argc, char **argv) {
    int s = 0, n = 0, r = 0, v = 0, m = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-' || !argv[i][1]) { fprintf(stderr, "uname: extra operand '%s'\n", argv[i]); return 1; }
        for (const char *f = argv[i] + 1; *f; f++)
            switch (*f) {
            case 'a': s = n = r = v = m = 1; break;
            case 's': s = 1; break;
            case 'n': n = 1; break;
            case 'r': r = 1; break;
            case 'v': v = 1; break;
            case 'm': case 'p': case 'i': m = 1; break;
            default: fprintf(stderr, "usage: uname [-asnrvm]\n"); return 1;
            }
    }
    if (!s && !n && !r && !v && !m) s = 1;
    char rel[32] = "2.0", ver[96] = "";
    int fd = open("/proc/version", O_RDONLY);
    if (fd >= 0) {
        char b[160];
        int k = read(fd, b, sizeof b - 1);
        close(fd);
        if (k > 0) {
            b[k] = 0;
            char *nl = strchr(b, '\n');
            if (nl) *nl = 0;
            char *sp = strchr(b, ' ');
            if (sp) { sscanf(sp + 1, "%31s", rel); char *bu = strstr(b, "built "); if (bu) strlcpy(ver, bu + 6, sizeof ver); }
        }
    }
    const char *out[5]; int c = 0;
    if (s) out[c++] = "VibeOS";
    if (n) out[c++] = "vibeos";
    if (r) out[c++] = rel;
    if (v) out[c++] = ver[0] ? ver : "#1";
    if (m) out[c++] = "i686";
    for (int i = 0; i < c; i++) printf("%s%s", i ? " " : "", out[i]);
    putchar('\n');
    return 0;
}
