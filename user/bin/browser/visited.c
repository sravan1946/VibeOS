/* visited.c - which URLs have been visited (for :visited). A set of 32-bit URL hashes (fragment dropped), kept in memory and appended to
 * ~/.config/browser/visited as hex lines so it survives restarts. Hashes, not URLs: no history leaks through the file or the matcher. */
#include "browser.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>

#define VCAP 4096                      /* entries (open addressing); when full the oldest half of the file is forgotten on the next start */
static uint32_t V[VCAP];               /* 0 = empty */
static int nv, loaded;

static uint32_t vhash(const char *url) {
    uint32_t h = 2166136261u;
    for (; *url && *url != '#'; url++) h = (h ^ (unsigned char)*url) * 16777619u;
    return h ? h : 1;
}
static const char *vpath(char *o, int cap) {
    const char *h = getenv("HOME");
    snprintf(o, cap, "%s/.config/browser/visited", h && *h ? h : "/home");
    return o;
}
static int vhas(uint32_t h) { for (unsigned i = h % VCAP, n = 0; n < VCAP && V[i]; i = (i + 1) % VCAP, n++) if (V[i] == h) return 1; return 0; }
static void vput(uint32_t h) {
    if (nv >= VCAP * 3 / 4 || vhas(h)) return;
    unsigned i = h % VCAP;
    while (V[i]) i = (i + 1) % VCAP;
    V[i] = h; nv++;
}
static void vload(void) {
    if (loaded) return;
    loaded = 1;
    char p[200], buf[4096];
    int fd = open(vpath(p, sizeof p), O_RDONLY);
    if (fd < 0) return;
    ssize_t r, have = 0;
    while ((r = read(fd, buf + have, sizeof buf - have - 1)) > 0) {
        have += r;
        buf[have] = 0;
        char *s = buf, *nl;
        while ((nl = strchr(s, '\n'))) { *nl = 0; vput((uint32_t)strtoul(s, 0, 16)); s = nl + 1; }
        have = strlen(s);
        memmove(buf, s, have);
    }
    close(fd);
}

int visited_url(const char *url) {
    if (!url || !*url) return 0;
    vload();
    return vhas(vhash(url));
}

void visited_add(const char *url) {
    if (!url || !*url || !url_is_web(url) || !strncasecmp(url, "data:", 5)) return;
    vload();
    uint32_t h = vhash(url);
    if (vhas(h)) return;
    vput(h);
    char p[200], line[16];
    vpath(p, sizeof p);
    for (char *d = p + 1; (d = strchr(d, '/')); *d++ = '/') { *d = 0; mkdir(p, 0755); *d = '/'; }   /* mkdir -p of the parents */
    int fd = open(p, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd < 0) return;
    int n = snprintf(line, sizeof line, "%x\n", h);
    if (write(fd, line, n) < 0) { }
    close(fd);
}
