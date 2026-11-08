/* tar {c|x|t}[vz][f ARCHIVE] [-C DIR] [PATH...]   ustar archives; z = gzip (extract also detects it by itself)
 *   tar czf a.tgz dir     tar tf a.tar     tar xzf a.tgz -C /tmp     no -f: stdin / stdout
 * Whole archive in memory. No links, owners or permissions on this OS: modes are stored (0644 / 0755), uid/gid are 0,
 * symlink and device entries from other tars are skipped with a warning. */
#include <vutil.h>
#include <dirent.h>
#include "image.h"

static int verbose, zflag, rc;
static uint8_t *ab; static size_t an, acap;            /* the archive being written */

static void put(const void *p, size_t n) {
    if (an + n > acap) { acap = (an + n) * 2 + 4096; if (!(ab = realloc(ab, acap))) { fputs("tar: out of memory\n", stderr); exit(1); } }
    memcpy(ab + an, p, n); an += n;
}

static void octal(char *f, int w, unsigned long long v) {
    for (int i = w - 2; i >= 0; i--, v >>= 3) f[i] = '0' + (v & 7);
    f[w - 1] = 0;
}

static void header(const char *name, int dir, uint32_t size, uint32_t mode, uint32_t mtime) {
    uint8_t h[512] = { 0 };
    size_t l = strlen(name);
    const char *base = name;
    char pre[160] = "";
    if (l > 100) {                                      /* ustar: split at a '/' so prefix <= 155 and name <= 100 */
        const char *s = name + l - 100;
        while (*s && *s != '/') s++;
        if (!*s || s - name > 155) { fprintf(stderr, "tar: %s: name too long, skipped\n", name); rc = 1; return; }
        snprintf(pre, sizeof pre, "%.*s", (int)(s - name), name);
        base = s + 1;
    }
    snprintf((char *)h, 100, "%s", base);
    octal((char *)h + 100, 8, mode & 07777);
    octal((char *)h + 108, 8, 0); octal((char *)h + 116, 8, 0);
    octal((char *)h + 124, 12, dir ? 0 : size);
    octal((char *)h + 136, 12, mtime);
    h[156] = dir ? '5' : '0';
    memcpy(h + 257, "ustar", 6); memcpy(h + 263, "00", 2);
    memcpy(h + 345, pre, strlen(pre));
    memset(h + 148, ' ', 8);
    unsigned sum = 0;
    for (int i = 0; i < 512; i++) sum += h[i];
    octal((char *)h + 148, 7, sum); h[155] = ' ';
    put(h, 512);
    if (verbose) fprintf(stderr, "%s\n", name);
}

static void add(const char *path, const char *name) {
    struct stat st;
    if (stat(path, &st)) { WARN("tar", path); rc = 1; return; }
    char nm[PATH_MAX + 2];
    while (*name == '/') name++;
    if (!*name) name = ".";
    if (S_ISDIR(st.st_mode)) {
        snprintf(nm, sizeof nm, "%s%s", name, name[strlen(name) - 1] == '/' ? "" : "/");
        header(nm, 1, 0, 0755, st.st_mtime);
        DIR *d = opendir(path);
        if (!d) { WARN("tar", path); rc = 1; return; }
        char **v = NULL; int n = 0;
        for (struct dirent *e; (e = readdir(d)); ) {      /* collect first: recursion keeps fds low and the order stable */
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            v = realloc(v, ++n * sizeof *v);
            v[n - 1] = strdup(e->d_name);
        }
        closedir(d);
        for (int i = 0; i < n; i++) {
            char p[PATH_MAX], q[PATH_MAX + 2];
            snprintf(p, sizeof p, "%s/%s", path, v[i]);
            snprintf(q, sizeof q, "%s%s", nm, v[i]);
            add(p, q);
            free(v[i]);
        }
        free(v);
    } else if (S_ISREG(st.st_mode)) {
        int fd = open(path, O_RDONLY);
        if (fd < 0) { WARN("tar", path); rc = 1; return; }
        header(name, 0, st.st_size, 0644 | (st.st_mode & 0111), st.st_mtime);
        size_t start = an, left = st.st_size;
        while (left) {
            uint8_t b[4096];
            ssize_t r = read(fd, b, left < sizeof b ? left : sizeof b);
            if (r <= 0) break;
            put(b, r); left -= r;
        }
        close(fd);
        if (left) { fprintf(stderr, "tar: %s: file shrank\n", path); rc = 1; }
        static const uint8_t z[512] = { 0 };
        while (left) { size_t k = left < 512 ? left : 512; put(z, k); left -= k; }   /* pad to the size the header promised */
        size_t body = an - start;
        if (body % 512) put(z, 512 - body % 512);
    } else { fprintf(stderr, "tar: %s: not a regular file or directory, skipped\n", path); rc = 1; }
}

static uint64_t oct(const char *f, int w) {
    uint64_t v = 0;
    if ((uint8_t)f[0] & 0x80) { for (int i = 1; i < w; i++) v = v << 8 | (uint8_t)f[i]; return v; }   /* GNU base-256 */
    for (int i = 0; i < w && f[i] == ' '; i++) f++, w--;
    for (int i = 0; i < w && f[i] >= '0' && f[i] <= '7'; i++) v = v << 3 | (f[i] - '0');
    return v;
}

static int unsafe(const char *p) {
    for (const char *s = p; *s; ) {
        if (s[0] == '.' && s[1] == '.' && (!s[2] || s[2] == '/')) return 1;
        while (*s && *s != '/') s++;
        while (*s == '/') s++;
    }
    return 0;
}

static void mkparents(const char *path) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
}

static void walk(const uint8_t *a, size_t n, int mode, const char *want[], int nwant) {
    char longname[PATH_MAX] = "";
    size_t pos = 0;
    while (pos + 512 <= n) {
        const uint8_t *h = a + pos;
        int zero = 1;
        for (int i = 0; i < 512 && zero; i++) zero = !h[i];
        if (zero) break;
        unsigned sum = 0, want_sum = oct((const char *)h + 148, 8);
        for (int i = 0; i < 512; i++) sum += i >= 148 && i < 156 ? ' ' : h[i];
        if (sum != want_sum) { fputs("tar: bad header checksum; stopping\n", stderr); rc = 1; return; }
        uint64_t size = oct((const char *)h + 124, 12);
        int type = h[156] ? h[156] : '0';
        char name[PATH_MAX + 160];
        if (longname[0]) { snprintf(name, sizeof name, "%s", longname); longname[0] = 0; }
        else if (h[345] && !memcmp(h + 257, "ustar", 5)) snprintf(name, sizeof name, "%.155s/%.100s", h + 345, h);
        else snprintf(name, sizeof name, "%.100s", h);
        const uint8_t *body = h + 512;
        if (pos + 512 + size > n) { fputs("tar: archive truncated\n", stderr); rc = 1; return; }
        pos += 512 + ((size + 511) & ~511ull);
        if (type == 'L') { snprintf(longname, sizeof longname, "%.*s", (int)(size < PATH_MAX - 1 ? size : PATH_MAX - 1), (const char *)body); continue; }
        if (type == 'x') {                               /* pax records "LEN key=value\n": only path matters */
            for (const char *r = (const char *)body, *e = r + size; r < e; ) {
                int len = atoi(r);
                const char *eq = memchr(r, '=', e - r);
                if (len <= 0 || r + len > e) break;
                if (eq && !strncmp(eq - 4, "path", 4) && eq[-5] == ' ') snprintf(longname, sizeof longname, "%.*s", (int)(r + len - 1 - eq - 1), eq + 1);
                r += len;
            }
            continue;
        }
        if (type == 'g') continue;
        if (nwant) {                                     /* only the named paths (and what is under them) */
            int hit = 0;
            for (int i = 0; i < nwant; i++) {
                size_t l = strlen(want[i]);
                while (l > 1 && want[i][l - 1] == '/') l--;
                if (!strncmp(name, want[i], l) && (!name[l] || name[l] == '/')) hit = 1;
            }
            if (!hit) continue;
        }
        const char *rel = name;
        while (*rel == '/') rel++;
        if (mode == 't') { puts(name); continue; }
        if (verbose) fprintf(stderr, "%s\n", name);
        if (unsafe(rel)) { fprintf(stderr, "tar: %s: refusing '..' in a member name\n", name); rc = 1; continue; }
        if (type == '5' || (type == '0' && name[strlen(name) - 1] == '/')) { mkparents(rel); if (mkdir(rel, 0755) && errno != EEXIST) { WARN("tar", rel); rc = 1; } continue; }
        if (type != '0' && type != '7') { fprintf(stderr, "tar: %s: skipped (type '%c' is not supported here)\n", name, type); rc = 1; continue; }
        mkparents(rel);
        int fd = open(rel, O_WRONLY | O_CREAT | O_TRUNC);
        if (fd < 0) { WARN("tar", rel); rc = 1; continue; }
        for (uint64_t o = 0; o < size; ) {
            ssize_t w = write(fd, body + o, size - o);
            if (w <= 0) { WARN("tar", rel); rc = 1; break; }
            o += w;
        }
        close(fd);
    }
}

int main(int argc, char **argv) {
    const char *f = NULL, *dir = NULL, *files[256];
    int nf = 0, mode = 0, i = 1;
    if (argc < 2) goto usage;
    for (const char *p = argv[i] + (argv[i][0] == '-'); *p; p++) {       /* "xzvf" bundles; -f / -C take the next argument */
        if (*p == 'c' || *p == 'x' || *p == 't') mode = *p;
        else if (*p == 'v') verbose = 1;
        else if (*p == 'z') zflag = 1;
        else if (*p == 'f' && i + 1 < argc) f = argv[++i];
        else if (*p == 'C' && i + 1 < argc) dir = argv[++i];
        else goto usage;
    }
    for (i++; i < argc; i++) {
        if (!strcmp(argv[i], "-C") && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) f = argv[++i];
        else if (nf < 256) files[nf++] = argv[i];
    }
    if (!mode) goto usage;
    if (f && !strcmp(f, "-")) f = NULL;
    if (f) { size_t l = strlen(f); if ((l > 3 && !strcmp(f + l - 3, ".gz")) || (l > 4 && !strcmp(f + l - 4, ".tgz"))) zflag = 1; }
    if (mode == 'c') {
        if (!nf) { fputs("tar: no files given (tar cf a.tar PATH...)\n", stderr); return 2; }
        if (dir && chdir(dir)) { WARN("tar", dir); return 1; }
        for (int k = 0; k < nf; k++) add(files[k], files[k]);
        static const uint8_t z[1024] = { 0 };
        put(z, 1024);
        uint8_t *o = ab; size_t on = an;
        if (zflag && !(o = gzip_buf(ab, an, &on))) { fputs("tar: out of memory\n", stderr); return 1; }
        int fd = f ? open(f, O_WRONLY | O_CREAT | O_TRUNC) : 1;
        if (fd < 0) { WARN("tar", f); return 1; }
        for (size_t w = 0; w < on; ) {
            ssize_t r = write(fd, o + w, on - w);
            if (r <= 0) { WARN("tar", f ? f : "stdout"); rc = 1; break; }
            w += r;
        }
        if (f) { close(fd); if (rc) unlink(f); }
        return rc;
    }
    int fd = f ? open(f, O_RDONLY) : 0;
    if (fd < 0) { WARN("tar", f); return 1; }
    size_t n;
    uint8_t *a = (uint8_t *)readall(fd, &n);
    if (!a) { WARN("tar", f ? f : "stdin"); return 1; }
    if (n > 2 && a[0] == 0x1f && a[1] == 0x8b) {
        uint8_t *o; size_t on;
        if (gunzip_buf(a, n, &o, &on)) { fputs("tar: corrupt gzip data\n", stderr); return 1; }
        a = o; n = on;
    }
    if (dir && chdir(dir)) { WARN("tar", dir); return 1; }
    walk(a, n, mode, files, nf);
    return rc;
usage:
    fputs("usage: tar c|x|t[v][z][f ARCHIVE] [-C DIR] [PATH...]\n", stderr);
    return 2;
}
