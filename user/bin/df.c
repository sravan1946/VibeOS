/* df [-ahk] [PATH...] : free space per filesystem (statfs); no PATH lists the mounts that have blocks */
#include <vutil.h>
#include <sys/statfs.h>

static int human, all;

static void cell(unsigned long long bytes, int w) {
    char b[24];
    if (!human) snprintf(b, sizeof b, "%llu", (bytes + 1023) / 1024);
    else if (bytes >= 1ull << 30) snprintf(b, sizeof b, "%u.%uG", (unsigned)(bytes >> 30), (unsigned)((bytes & 0x3FFFFFFF) * 10 >> 30));
    else if (bytes >= 1 << 20) snprintf(b, sizeof b, "%u.%uM", (unsigned)(bytes >> 20), (unsigned)((bytes & 0xFFFFF) * 10 >> 20));
    else snprintf(b, sizeof b, "%uK", (unsigned)((bytes + 1023) >> 10));
    printf(" %*s", w, b);
}

/* ponytail: no mount-table syscall; these are the three mounts main.c/devfs.c make */
static const char *const mounts[] = { "/", "/dev", "/proc" };

static const char *mount_of(const char *path) {
    char cwd[PATH_MAX], full[PATH_MAX * 2];
    if (path[0] != '/' && getcwd(cwd, sizeof cwd)) { snprintf(full, sizeof full, "%s/%s", cwd, path); path = full; }
    for (int i = 1; i < 3; i++) {
        size_t n = strlen(mounts[i]);
        if (!strncmp(path, mounts[i], n) && (!path[n] || path[n] == '/')) return mounts[i];
    }
    return "/";
}

static int show(const char *path, const char *mnt, int named) {
    struct statfs f;
    if (statfs(path, &f) < 0) { WARN("df", path); return 1; }
    if (!f.f_blocks && !all && !named) return 0;
    const char *name = f.f_type == EXT2_SUPER_MAGIC ? "ext2" : f.f_type == PROC_SUPER_MAGIC ? "proc" :
                       f.f_type == PIPEFS_MAGIC ? "pipefs" : "devfs";
    unsigned long long bs = f.f_bsize, used = f.f_blocks - f.f_bfree, avail = f.f_bavail;
    unsigned pct = used + avail ? (unsigned)((used * 100 + used + avail - 1) / (used + avail)) : 0;
    printf("%-12s", name);
    cell(f.f_blocks * bs, 10); cell(used * bs, 10); cell(avail * bs, 10);
    if (used + avail) printf(" %3u%%", pct); else printf("    -");
    printf(" %s\n", mnt);
    return 0;
}

int main(int argc, char **argv) {
    int i = 1, rc = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (const char *o = argv[i] + 1; *o; o++) {
            if (*o == 'h') human = 1;
            else if (*o == 'k') human = 0;
            else if (*o == 'a') all = 1;
            else { fprintf(stderr, "usage: df [-ahk] [PATH...]\n"); return 1; }
        }
    printf("%-12s %10s %10s %10s %4s %s\n", "Filesystem", human ? "Size" : "1K-blocks", "Used", "Avail", "Use%", "Mounted on");
    if (i == argc) for (int m = 0; m < 3; m++) rc |= show(mounts[m], mounts[m], 0);
    for (; i < argc; i++) rc |= show(argv[i], mount_of(argv[i]), 1);
    return rc;
}
