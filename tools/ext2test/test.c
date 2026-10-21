/* test.c - exercise kernel/fs/ext2.c on the host against an image file.
 * usage: t <image> <srcdir> <outdir> <blocksize>
 * Writes <outdir>/manifest.txt ("F idx path" = file expected on disk as <outdir>/idx,
 * "A idx path" = must be absent) for run.sh to check with debugfs. */
#include "vfs.h"

/* ---- kernel shims ---- */
volatile uint64_t ticks_ms;
void *kmalloc(size_t n) { return malloc(n); }
void *kzalloc(size_t n) { return calloc(1, n); }
void kfree(void *p) { free(p); }
void kprintf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); }
NORETURN void panic(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); abort(); }
void vnode_put(struct vnode *v) { if (--v->refs == 0) v->ops->release(v); }
uint32_t ext2_now(void) { return 1700000000; }

static int bd_rw(struct blockdev *d, int wr, uint32_t lba, uint32_t n, void *buf) {
    FILE *f = d->priv;
    if (lba + n > d->sectors || lba + n < lba) return -EIO;
    if (fseeko(f, (off_t)lba * 512, SEEK_SET)) return -EIO;
    if (wr) { if (fwrite(buf, 512, n, f) != n || fflush(f)) return -EIO; }
    else if (fread(buf, 512, n, f) != n) return -EIO;
    return 0;
}
static int bd_read(struct blockdev *d, uint32_t lba, uint32_t n, void *b) { return bd_rw(d, 0, lba, n, b); }
static int bd_write(struct blockdev *d, uint32_t lba, uint32_t n, const void *b) { return bd_rw(d, 1, lba, n, (void *)b); }

static struct blockdev *open_img(const char *path) {
    FILE *f = fopen(path, "r+b");
    if (!f) { perror(path); exit(2); }
    fseeko(f, 0, SEEK_END);
    struct blockdev *d = calloc(1, sizeof *d);
    d->sectors = ftello(f) / 512;
    d->read = bd_read; d->write = bd_write; d->priv = f;
    return d;
}

/* ---- test helpers ---- */
static int is_dots_name(const char *s) { return !strcmp(s, ".") || !strcmp(s, ".."); }
static int fails, checks;
#define CK(c, ...) do { checks++; if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static struct vnode *root;
static const char *srcdir;
static uint32_t bsz;

static struct vnode *walk(const char *path) {
    struct vnode *v = root, *n;
    char buf[512], *p, *save = NULL;
    strcpy(buf, path);
    vnode_get(v);
    for (p = strtok_r(buf, "/", &save); p; p = strtok_r(NULL, "/", &save)) {
        if (v->ops->lookup(v, p, &n) < 0) { vnode_put(v); return NULL; }
        vnode_put(v); v = n;
    }
    return v;
}

/* parent dir vnode of path, leaf name copied to leaf */
static struct vnode *parent(const char *path, char *leaf) {
    char buf[512];
    strcpy(buf, path);
    char *s = strrchr(buf, '/');
    if (!s) { strcpy(leaf, buf); return walk(""); }
    *s = 0; strcpy(leaf, s + 1);
    return walk(buf);
}

static int mk(const char *path, uint32_t mode) {
    char leaf[512];
    struct vnode *d = parent(path, leaf), *n;
    if (!d) return -ENOENT;
    int r = d->ops->create(d, leaf, mode, &n);
    if (r >= 0) vnode_put(n);
    vnode_put(d);
    return r;
}
static int rm(const char *path, bool is_dir) {
    char leaf[512];
    struct vnode *d = parent(path, leaf);
    if (!d) return -ENOENT;
    int r = d->ops->unlink(d, leaf, is_dir);
    vnode_put(d);
    return r;
}
static int mv(const char *a, const char *b) {
    char la[512], lb[512];
    struct vnode *da = parent(a, la), *db = parent(b, lb);
    int r = !da || !db ? -ENOENT : da->ops->rename(da, la, db, lb);
    if (da) vnode_put(da);
    if (db) vnode_put(db);
    return r;
}

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static uint8_t *pattern(size_t n, uint32_t seed) {
    uint8_t *b = malloc(n ? n : 1);
    rng = seed;
    for (size_t i = 0; i < n; i++) b[i] = rnd() >> 11;
    return b;
}

/* read a whole file in odd-sized chunks */
static uint8_t *read_all(struct vnode *v, uint32_t *len) {
    uint8_t *b = malloc(v->size + 1);
    uint32_t off = 0;
    for (;;) {
        int r = v->ops->read(v, off, b + off, MIN(3001u, v->size - off));
        if (r <= 0) { CK(r == 0, "read error %d", r); break; }
        off += r;
    }
    *len = off;
    return b;
}

/* ---- model of file contents ---- */
struct model { char path[160]; uint8_t *d; size_t n; int live; } M[512];
static int nm;
static struct model *mfind(const char *p) {
    for (int i = 0; i < nm; i++) if (M[i].live && !strcmp(M[i].path, p)) return &M[i];
    return NULL;
}
static struct model *mget(const char *p) {
    struct model *m = mfind(p);
    if (m) return m;
    m = &M[nm++];
    strcpy(m->path, p); m->live = 1; m->d = NULL; m->n = 0;
    return m;
}
static void mwrite(const char *p, size_t off, const void *b, size_t n) {
    struct model *m = mget(p);
    if (off + n > m->n) { m->d = realloc(m->d, off + n); memset(m->d + m->n, 0, off + n - m->n); m->n = off + n; }
    memcpy(m->d + off, b, n);
}
static void mtrunc(const char *p, size_t n) {
    struct model *m = mget(p);
    if (n > m->n) { m->d = realloc(m->d, n); memset(m->d + m->n, 0, n - m->n); }
    m->n = n;
}
static void mload(const char *p) {     /* model starts as the host source file */
    char path[600];
    snprintf(path, sizeof path, "%s/%s", srcdir, p);
    FILE *f = fopen(path, "rb");
    CK(f != NULL, "no host file %s", path);
    if (!f) return;
    uint8_t buf[65536];
    size_t n;
    mget(p);
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) mwrite(p, mfind(p)->n, buf, n);
    fclose(f);
}
static void mdel(const char *p) { struct model *m = mfind(p); if (m) { m->live = 0; free(m->d); m->d = NULL; } snprintf(M[nm].path, 160, "%s", p); M[nm].live = 0; M[nm].n = (size_t)-1; nm++; }
static void mmove(const char *a, const char *b) {
    struct model *s = mfind(a), *t = mfind(b);
    if (t) { t->live = 0; free(t->d); }
    if (s) { snprintf(s->path, 160, "%s", b); }
    snprintf(M[nm].path, 160, "%s", a); M[nm].live = 0; M[nm].n = (size_t)-1; nm++;
}

/* write via the fs in `chunk`-sized pieces, creating the file if needed; keeps the model in sync */
static void wr(const char *path, uint32_t off, const void *buf, uint32_t len, uint32_t chunk) {
    struct vnode *v = walk(path);
    if (!v) { CK(mk(path, S_IFREG | 0644) >= 0, "create %s", path); v = walk(path); }
    if (!v) return;
    for (uint32_t done = 0; done < len; ) {
        uint32_t c = MIN(chunk, len - done);
        int r = v->ops->write(v, off + done, (const uint8_t *)buf + done, c);
        CK(r == (int)c, "write %s @%u: %d", path, off + done, r);
        if (r <= 0) break;
        done += r;
    }
    vnode_put(v);
    mwrite(path, off, buf, len);
}

static void verify_model(void) {
    for (int i = 0; i < nm; i++) {
        if (!M[i].live) continue;
        struct vnode *v = walk(M[i].path);
        CK(v != NULL, "missing %s", M[i].path);
        if (!v) continue;
        uint32_t n;
        uint8_t *b = read_all(v, &n);
        CK(v->size == M[i].n && n == M[i].n, "%s size %u/%u want %zu", M[i].path, v->size, n, M[i].n);
        CK(n == M[i].n && !memcmp(b, M[i].d, n), "%s content mismatch", M[i].path);
        free(b);
        vnode_put(v);
    }
}

/* ---- tree walk ---- */
static int nfiles;
static void verify_tree(struct vnode *d, const char *prefix) {
    uint32_t cookie = 0;
    struct dirent de;
    while (d->ops->readdir(d, &cookie, &de) == 1) {
        if (is_dots_name(de.d_name)) continue;
        char path[512];
        snprintf(path, sizeof path, "%s%s", prefix, de.d_name);
        struct vnode *v;
        CK(d->ops->lookup(d, de.d_name, &v) == 0, "lookup %s", path);
        if (S_ISDIR(v->mode)) {
            CK(de.d_type == DT_DIR, "d_type %s", path);
            if (strcmp(path, "lost+found")) {
                strcat(path, "/");
                verify_tree(v, path);
            }
        } else {
            char hp[600];
            snprintf(hp, sizeof hp, "%s/%s", srcdir, path);
            FILE *f = fopen(hp, "rb");
            CK(f != NULL, "host file missing: %s", hp);
            uint32_t n;
            uint8_t *b = read_all(v, &n);
            if (f) {
                uint8_t *h = malloc(n + 1);
                size_t hn = fread(h, 1, n + 1, f);
                CK(hn == n && !memcmp(h, b, n), "tree mismatch %s", path);
                free(h); fclose(f);
            }
            free(b);
            nfiles++;
        }
        vnode_put(v);
    }
}

static int list_names(const char *path, char names[][260], int max) {
    struct vnode *d = walk(path);
    uint32_t cookie = 0;
    struct dirent de;
    int n = 0;
    if (!d) return -1;
    while (d->ops->readdir(d, &cookie, &de) == 1)
        if (!is_dots_name(de.d_name) && n < max) strcpy(names[n++], de.d_name);
    vnode_put(d);
    return n;
}

static void tests(void) {
    char name[300];
    uint8_t *p;

    /* 1. create / write / append */
    CK(mk("new.txt", S_IFREG | 0644) >= 0, "create new.txt");
    CK(mk("new.txt", S_IFREG | 0644) == -EEXIST, "dup create");
    wr("new.txt", 0, "hello", 5, 5);
    wr("new.txt", 5, " world", 6, 2);

    /* 2. dirs, multi-MiB file in odd chunks */
    CK(mk("d1", S_IFDIR | 0755) >= 0, "mkdir d1");
    CK(mk("d1/d2", S_IFDIR | 0755) >= 0, "mkdir d1/d2");
    struct vnode *d1 = walk("d1");
    CK(d1 && d1->nlink == 3, "d1 nlink %u", d1 ? d1->nlink : 0);
    vnode_put(d1);
    p = pattern(5 * 1024 * 1024 + 123, 1);
    wr("d1/d2/big.bin", 0, p, 5 * 1024 * 1024 + 123, 7777);
    free(p);
    wr("d1/small", 0, "x", 1, 1);

    /* 3. modify files that came from mke2fs */
    mload("sub/a.txt");
    wr("sub/a.txt", mfind("sub/a.txt")->n, "appended\n", 9, 9);
    mload("big300k.bin");
    p = pattern(20000, 2);
    wr("big300k.bin", 270000, p, 20000, 4099);
    wr("big300k.bin", 100, p, 5000, 5000);
    free(p);
    mload("small.txt");
    wr("small.txt", 0, "HELLO", 5, 5);
    mload("empty.txt");
    wr("empty.txt", 0, "not empty any more", 18, 18);

    /* 4. sparse + triple indirect */
    CK(mk("sparse.bin", S_IFREG | 0600) >= 0, "create sparse");
    wr("sparse.bin", 1024 * 1024 + 5, "abc", 3, 3);
    {
        struct vnode *v = walk("sparse.bin");
        uint8_t z[8192], zz[8192];
        memset(zz, 0, sizeof zz);
        CK(v->ops->read(v, 5000, z, sizeof z) == (int)sizeof z && !memcmp(z, zz, sizeof z), "hole reads zero");
        CK(v->size == 1024 * 1024 + 8, "sparse size %u", v->size);
        vnode_put(v);
    }
    if (bsz <= 2048) {
        uint32_t P = bsz / 4;
        uint32_t off = (12 + P + P * P + 3) * bsz + 17;
        CK(mk("triple.bin", S_IFREG | 0644) >= 0, "create triple");
        struct vnode *v = walk("triple.bin");
        CK(v->ops->write(v, off, "TRIPLE!", 7) == 7, "triple write");
        char b[8] = {0};
        CK(v->ops->read(v, off, b, 7) == 7 && !strcmp(b, "TRIPLE!"), "triple read");
        CK(v->ops->truncate(v, 1000) == 0 && v->size == 1000, "triple truncate");
        vnode_put(v);
        CK(rm("triple.bin", false) == 0, "triple rm");
        mdel("triple.bin");
    }

    /* 5. truncate: shrink, grow (zeros), to 0, rewrite */
    p = pattern(600000, 3);
    wr("trunc.bin", 0, p, 600000, 65536);
    {
        struct vnode *v = walk("trunc.bin");
        CK(v->ops->truncate(v, 300000) == 0, "trunc 1"); mtrunc("trunc.bin", 300000);
        CK(v->ops->truncate(v, 12345) == 0, "trunc 2"); mtrunc("trunc.bin", 12345);
        CK(v->ops->truncate(v, 40000) == 0, "trunc grow"); mtrunc("trunc.bin", 40000);
        vnode_put(v);
        verify_model();
        wr("trunc.bin", 39990, "tail-bytes", 10, 10);
        mload("sub/deep/b.bin");
        v = walk("sub/deep/b.bin");
        CK(v->ops->truncate(v, 0) == 0, "trunc 0"); mtrunc("sub/deep/b.bin", 0);
        vnode_put(v);
        wr("sub/deep/b.bin", 0, "reborn", 6, 6);
    }
    free(p);

    /* 6. rename */
    CK(mv("new.txt", "d1/renamed.txt") == 0, "rename file"); mmove("new.txt", "d1/renamed.txt");
    CK(walk("new.txt") == NULL, "old name gone");
    wr("rep_src", 0, "source", 6, 6);
    wr("rep_dst", 0, "destination is longer", 21, 21);
    CK(mv("rep_src", "rep_dst") == 0, "rename replace"); mmove("rep_src", "rep_dst");
    CK(mk("mv1", S_IFDIR | 0755) >= 0 && mk("mv1/sub", S_IFDIR | 0755) >= 0, "mkdir mv1/sub");
    wr("mv1/sub/f", 0, "deep file", 9, 9);
    struct vnode *a = walk("d1"), *b = walk("mv1");
    uint32_t an = a->nlink, bn = b->nlink;
    CK(mv("mv1/sub", "d1/sub2") == 0, "rename dir across parents"); mmove("mv1/sub/f", "d1/sub2/f");
    CK(a->nlink == an + 1 && b->nlink == bn - 1, "dir nlinks after move: %u %u", a->nlink, b->nlink);
    struct vnode *s2 = walk("d1/sub2"), *dd;
    CK(s2 && s2->ops->lookup(s2, "..", &dd) == 0 && dd->ino == a->ino, "'..' updated");
    if (dd) vnode_put(dd);
    vnode_put(s2); vnode_put(a); vnode_put(b);
    CK(mk("e1", S_IFDIR | 0755) >= 0 && mk("e2", S_IFDIR | 0755) >= 0, "mkdir e1 e2");
    CK(mv("e1", "e2") == 0, "dir over empty dir");
    CK(walk("e1") == NULL, "e1 gone");
    dd = walk("e2"); CK(dd != NULL, "e2 stays"); if (dd) vnode_put(dd);
    CK(mk("ne", S_IFDIR | 0755) >= 0 && mk("ne/x", S_IFREG | 0644) >= 0 && mk("ne2", S_IFDIR | 0755) >= 0, "ne setup");
    CK(mv("ne2", "ne") == -ENOTEMPTY, "dir over non-empty dir");
    CK(mv("d1/small", "ne2") == -ENOTDIR || mv("d1/small", "ne2") == -EISDIR, "file over dir");
    CK(mv("ne2", "d1/small") == -ENOTDIR, "dir over file");
    CK(mk("loop", S_IFDIR | 0755) >= 0 && mk("loop/inner", S_IFDIR | 0755) >= 0, "loop setup");
    CK(mv("loop", "loop/inner/loop") == -EINVAL, "dir into own subtree");
    CK(mv("loop/inner", "loop/inner2") == 0, "rename in same dir");
    CK(mv("nonexist", "zzz") == -ENOENT, "rename missing");

    /* 7. unlink / rmdir semantics */
    CK(rm("ne", false) == -EISDIR, "unlink dir");
    CK(rm("d1/small", true) == -ENOTDIR, "rmdir file");
    CK(rm("ne", true) == -ENOTEMPTY, "rmdir non-empty");
    CK(rm("ne/x", false) == 0 && rm("ne", true) == 0, "remove ne/x, ne");
    CK(rm("ne2", true) == 0 && rm("e2", true) == 0 && rm("loop/inner2", true) == 0 && rm("loop", true) == 0, "rmdirs");
    CK(rm("d1/small", false) == 0, "unlink file"); mdel("d1/small");
    CK(rm("nothing", false) == -ENOENT, "unlink missing");
    CK(rm("small.txt", false) == 0, "unlink host file"); mdel("small.txt");
    CK(rm("emptydir", true) == 0, "rmdir emptydir");
    CK(walk("emptydir") == NULL && walk("ne") == NULL, "gone");

    /* 8. many entries: directory grows over several blocks, merge/split, long name */
    CK(mk("many", S_IFDIR | 0755) >= 0, "mkdir many");
    for (int i = 0; i < 400; i++) {
        snprintf(name, sizeof name, "many/f%03d", i);
        CK(mk(name, S_IFREG | 0644) >= 0, "create %s", name);
        if (i % 50 == 0) wr(name, 0, name, strlen(name), 3);
    }
    for (int i = 1; i < 400; i += 2) { snprintf(name, sizeof name, "many/f%03d", i); CK(rm(name, false) == 0, "rm %s", name); }
    for (int i = 0; i < 100; i++) { snprintf(name, sizeof name, "many/g%03d", i); CK(mk(name, S_IFREG | 0644) >= 0, "create %s", name); }
    memset(name, 'L', 255); name[255] = 0;
    char lp[600]; snprintf(lp, sizeof lp, "many/%s", name);
    CK(mk(lp, S_IFREG | 0644) >= 0, "255-char name");
    snprintf(lp, sizeof lp, "many/%sX", name);
    CK(mk(lp, S_IFREG | 0644) == -ENAMETOOLONG, "256-char name");
    CK(mk("many/a/b", S_IFREG | 0644) == -ENOENT, "create in missing dir");
    {
        static char names[1000][260];
        int n = list_names("many", names, 1000);
        CK(n == 200 + 100 + 1, "many has %d entries", n);
        int seen_f = 0, seen_g = 0, seen_l = 0, bad = 0;
        for (int i = 0; i < n; i++) {
            if (names[i][0] == 'f') { seen_f++; bad += atoi(names[i] + 1) % 2; }
            else if (names[i][0] == 'g') seen_g++;
            else if (names[i][0] == 'L') seen_l++;
        }
        CK(seen_f == 200 && seen_g == 100 && seen_l == 1 && !bad, "many contents f%d g%d L%d bad%d", seen_f, seen_g, seen_l, bad);
    }
    mdel("many/f001");

    /* 9. unlink while open: data survives until the last put */
    p = pattern(100000, 4);
    wr("tmpopen", 0, p, 100000, 100000);
    {
        struct vnode *v = walk("tmpopen");
        CK(rm("tmpopen", false) == 0, "unlink open file");
        mdel("tmpopen");
        uint32_t n;
        uint8_t *b = read_all(v, &n);
        CK(n == 100000 && !memcmp(b, p, n), "open-unlinked file readable");
        free(b);
        vnode_put(v);
    }
    free(p);
    mdel("tmpopen");

    /* 10. out of space, then recover it */
    {
        uint8_t *chunk = pattern(1 << 20, 5);
        CK(mk("fill", S_IFREG | 0644) >= 0, "create fill");
        struct vnode *v = walk("fill");
        uint32_t off = 0;
        int r;
        while ((r = v->ops->write(v, off, chunk, 1 << 20)) > 0) off += r;
        CK(r == -ENOSPC, "fill ended with %d", r);
        CK(v->size == off, "fill size");
        vnode_put(v);
        CK(rm("fill", false) == 0, "rm fill");
        CK(mk("after", S_IFREG | 0644) >= 0, "create after full");
        wr("after", 0, chunk, 1 << 20, 1 << 20);
        free(chunk);
        mdel("fill");
    }
}

int main(int argc, char **argv) {
    if (argc != 5) { fprintf(stderr, "usage: %s image srcdir outdir bs\n", argv[0]); return 2; }
    srcdir = argv[2];
    bsz = atoi(argv[4]);
    struct blockdev *dev = open_img(argv[1]);
    CK(ext2_mount(dev, &root) == 0, "mount");
    if (fails) return 1;

    verify_tree(root, "");
    char cmd[700];
    snprintf(cmd, sizeof cmd, "find %s -type f | wc -l", srcdir);
    FILE *p = popen(cmd, "r");
    int hostfiles = 0;
    if (fscanf(p, "%d", &hostfiles) != 1) hostfiles = -1;
    pclose(p);
    CK(nfiles == hostfiles, "walked %d files, host has %d", nfiles, hostfiles);
    printf("initial tree: %d files match the host copy\n", nfiles);

    tests();
    verify_model();
    CK(ext2_sync_all() == 0, "sync_all");

    /* a second mount of the same image must see everything (nothing only in caches) */
    struct vnode *old = root;
    struct blockdev *dev2 = open_img(argv[1]);
    CK(ext2_mount(dev2, &root) == 0, "remount");
    verify_model();
    root = old;

    char path[700];
    snprintf(path, sizeof path, "%s/manifest.txt", argv[3]);
    FILE *mf = fopen(path, "w");
    int idx = 0;
    for (int i = 0; i < nm; i++) {
        if (M[i].live) {
            snprintf(path, sizeof path, "%s/%d", argv[3], idx);
            FILE *f = fopen(path, "wb");
            fwrite(M[i].d, 1, M[i].n, f);
            fclose(f);
            fprintf(mf, "F %d %s\n", idx++, M[i].path);
        } else if (M[i].n == (size_t)-1 && !mfind(M[i].path))
            fprintf(mf, "A %d %s\n", idx++, M[i].path);
    }
    fclose(mf);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
