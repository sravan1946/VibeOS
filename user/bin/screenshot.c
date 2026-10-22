/* screenshot [file] - save the composed screen (read from /dev/screen) as PNG; PrtSc runs this.
 * Default name: /home/Pictures/screenshot-N.png. Posts a desktop notification when run from the desktop. */
#include <gui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "image.h"

int main(int argc, char **argv) {
    int fd = open("/dev/screen", O_RDONLY);
    uint32_t hdr[2];
    if (fd < 0 || read(fd, hdr, 8) != 8) { fprintf(stderr, "screenshot: no /dev/screen\n"); return 1; }
    int w = hdr[0], h = hdr[1];
    size_t n = (size_t)w * h * 4, got = 0;
    uint32_t *px = malloc(n);
    while (px && got < n) {
        int r = read(fd, (char *)px + got, n - got);
        if (r <= 0) break;
        got += r;
    }
    close(fd);
    if (!px || got < n) { fprintf(stderr, "screenshot: short read\n"); return 1; }
    for (size_t i = 0; i < (size_t)w * h; i++) px[i] |= 0xFF000000u;
    char path[PATH_MAX];
    if (argc > 1) snprintf(path, sizeof path, "%s", argv[1]);
    else {
        mkdir("/home/Pictures", 0755);
        struct stat st;
        for (int i = 1; i < 10000; i++) {
            snprintf(path, sizeof path, "/home/Pictures/screenshot-%d.png", i);
            if (stat(path, &st)) break;
        }
    }
    bool ok = image_save_png(path, px, w, h) == 0;
    char msg[PATH_MAX + 32];
    snprintf(msg, sizeof msg, ok ? "Saved %s" : "Could not save %s", path);
    if (ok) puts(msg); else fprintf(stderr, "%s\n", msg);
    if (!isatty(1)) gui_notify(ok ? "Screenshot saved" : "Screenshot failed", path, ok ? "good" : "bad");
    free(px);
    return ok ? 0 : 1;
}
