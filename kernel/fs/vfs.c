/* vfs.c - path resolution, mounts, open files */
#include "vfs.h"

#define MAX_MOUNTS 8
static struct { char path[64]; struct vnode *root; } mounts[MAX_MOUNTS];
static int nmounts;

void vnode_put(struct vnode *v) {
    if (!v) return;
    assert(v->refs > 0);
    if (--v->refs == 0 && v->ops->release) v->ops->release(v);
}

int vfs_mount(const char *path, struct vnode *root) {
    if (nmounts == MAX_MOUNTS) return -ENOMEM;
    strlcpy(mounts[nmounts].path, path, sizeof mounts[0].path);
    mounts[nmounts++].root = root;
    return 0;
}

/* join cwd + path and resolve ".", ".." and duplicate slashes */
int vfs_canon(const char *cwd, const char *path, char *out, size_t n) {
    char tmp[PATH_MAX * 2];
    if (!path || !*path) return -ENOENT;
    if (path[0] == '/') { if (strlcpy(tmp, path, sizeof tmp) >= sizeof tmp) return -ENAMETOOLONG; }
    else if ((size_t)snprintf(tmp, sizeof tmp, "%s/%s", cwd, path) >= sizeof tmp) return -ENAMETOOLONG;
    size_t o = 0;
    char *p = tmp;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char *s = p;
        while (*p && *p != '/') p++;
        size_t len = p - s;
        if (len > NAME_MAX) return -ENAMETOOLONG;
        if (len == 1 && s[0] == '.') continue;
        if (len == 2 && s[0] == '.' && s[1] == '.') {
            while (o > 0 && out[o - 1] != '/') o--;
            if (o > 0) o--;
            continue;
        }
        if (o + len + 2 > n) return -ENAMETOOLONG;
        out[o++] = '/';
        memcpy(out + o, s, len);
        o += len;
    }
    if (o == 0) out[o++] = '/';
    out[o] = 0;
    return 0;
}

/* walk a canonical path from the root of the longest matching mount */
static int walk(const char *cpath, struct vnode **out) {
    int best = -1; size_t blen = 0;
    for (int i = 0; i < nmounts; i++) {
        size_t l = strlen(mounts[i].path);
        if (l == 1 || (!strncmp(cpath, mounts[i].path, l) && (cpath[l] == '/' || !cpath[l])))
            if (best < 0 || l > blen) { best = i; blen = l; }
    }
    if (best < 0) return -ENOENT;
    struct vnode *v = mounts[best].root;
    vnode_get(v);
    const char *p = cpath + (blen == 1 ? 0 : blen);
    char name[NAME_MAX + 1];
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        size_t len = 0;
        while (p[len] && p[len] != '/') len++;
        memcpy(name, p, len); name[len] = 0;
        p += len;
        if (!S_ISDIR(v->mode) || !v->ops->lookup) { vnode_put(v); return -ENOTDIR; }
        struct vnode *nx;
        int r = v->ops->lookup(v, name, &nx);
        vnode_put(v);
        if (r < 0) return r;
        v = nx;
    }
    *out = v;
    return 0;
}

int vfs_lookup(const char *cwd, const char *path, struct vnode **out) {
    char c[PATH_MAX];
    int r = vfs_canon(cwd, path, c, sizeof c);
    return r < 0 ? r : walk(c, out);
}

/* split a path into parent vnode + last name */
static int parent_of(const char *cwd, const char *path, struct vnode **dir, char *name) {
    char c[PATH_MAX];
    int r = vfs_canon(cwd, path, c, sizeof c);
    if (r < 0) return r;
    if (!strcmp(c, "/")) return -EBUSY;
    char *slash = strrchr(c, '/');
    strlcpy(name, slash + 1, NAME_MAX + 1);
    if (slash == c) slash[1] = 0; else *slash = 0;
    if ((r = walk(c, dir)) < 0) return r;
    if (!S_ISDIR((*dir)->mode)) { vnode_put(*dir); return -ENOTDIR; }
    return 0;
}

struct file *vfs_open_vnode(struct vnode *v, int flags) {
    struct file *f = kzalloc(sizeof *f);
    if (!f) return 0;
    vnode_get(v);
    f->vn = v; f->flags = flags; f->refs = 1;
    return f;
}

int vfs_open(const char *cwd, const char *path, int flags, struct file **out) {
    struct vnode *v;
    int r = vfs_lookup(cwd, path, &v);
    if (r == -ENOENT && (flags & O_CREAT)) {
        struct vnode *dir; char name[NAME_MAX + 1];
        if ((r = parent_of(cwd, path, &dir, name)) < 0) return r;
        r = dir->ops->create ? dir->ops->create(dir, name, S_IFREG | 0644, &v) : -EROFS;
        vnode_put(dir);
        if (r < 0) return r;
    } else if (r < 0) {
        return r;
    } else if ((flags & O_CREAT) && (flags & O_EXCL)) {
        vnode_put(v); return -EEXIST;
    }
    if (S_ISDIR(v->mode) && (flags & O_ACCMODE) != O_RDONLY) { vnode_put(v); return -EISDIR; }
    if ((flags & O_DIRECTORY) && !S_ISDIR(v->mode)) { vnode_put(v); return -ENOTDIR; }
    if ((flags & O_TRUNC) && S_ISREG(v->mode) && (flags & O_ACCMODE) != O_RDONLY && v->ops->truncate)
        if ((r = v->ops->truncate(v, 0)) < 0) { vnode_put(v); return r; }
    struct file *f = vfs_open_vnode(v, flags);
    vnode_put(v);
    if (!f) return -ENOMEM;
    *out = f;
    return 0;
}

void vfs_close(struct file *f) {
    if (--f->refs > 0) return;
    if (f->vn->ops->close) f->vn->ops->close(f->vn, f->flags);
    vnode_put(f->vn);
    kfree(f);
}

int vfs_read(struct file *f, void *buf, uint32_t n) {
    if ((f->flags & O_ACCMODE) == O_WRONLY) return -EBADF;
    if (S_ISDIR(f->vn->mode)) return -EISDIR;
    if (!f->vn->ops->read) return -EINVAL;
    int r = f->vn->ops->read(f->vn, f->off, buf, n);
    if (r > 0 && S_ISREG(f->vn->mode)) f->off += r;
    return r;
}

int vfs_write(struct file *f, const void *buf, uint32_t n) {
    if ((f->flags & O_ACCMODE) == O_RDONLY) return -EBADF;
    if (!f->vn->ops->write) return -EINVAL;
    if ((f->flags & O_APPEND) && S_ISREG(f->vn->mode)) f->off = f->vn->size;
    int r = f->vn->ops->write(f->vn, f->off, buf, n);
    if (r > 0 && S_ISREG(f->vn->mode)) f->off += r;
    return r;
}

int vfs_seek(struct file *f, int32_t off, int whence) {
    if (!S_ISREG(f->vn->mode) && !S_ISDIR(f->vn->mode)) return -ESPIPE;
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (int64_t)f->off : whence == SEEK_END ? (int64_t)f->vn->size : -1;
    if (base < 0 || base + off < 0) return -EINVAL;
    f->off = base + off;
    if (S_ISDIR(f->vn->mode) && whence == SEEK_SET && off == 0) f->dir_cookie = 0;
    return f->off;
}

int vfs_stat_vnode(struct vnode *v, struct stat *st) {
    memset(st, 0, sizeof *st);
    st->st_ino = v->ino; st->st_mode = v->mode; st->st_nlink = v->nlink ? v->nlink : 1;
    st->st_size = v->size; st->st_mtime = v->mtime; st->st_ctime = v->ctime; st->st_atime = v->atime;
    st->st_blocks = (v->size + 511) / 512;
    return 0;
}

int vfs_mkdir(const char *cwd, const char *path) {
    struct vnode *dir, *v; char name[NAME_MAX + 1];
    int r = parent_of(cwd, path, &dir, name);
    if (r < 0) return r;
    r = dir->ops->create ? dir->ops->create(dir, name, S_IFDIR | 0755, &v) : -EROFS;
    vnode_put(dir);
    if (r == 0) vnode_put(v);
    return r;
}

int vfs_unlink(const char *cwd, const char *path, bool is_dir) {
    struct vnode *dir; char name[NAME_MAX + 1];
    int r = parent_of(cwd, path, &dir, name);
    if (r < 0) return r;
    r = dir->ops->unlink ? dir->ops->unlink(dir, name, is_dir) : -EROFS;
    vnode_put(dir);
    return r;
}

int vfs_rename(const char *cwd, const char *oldp, const char *newp) {
    struct vnode *od, *nd; char on[NAME_MAX + 1], nn[NAME_MAX + 1];
    int r = parent_of(cwd, oldp, &od, on);
    if (r < 0) return r;
    if ((r = parent_of(cwd, newp, &nd, nn)) < 0) { vnode_put(od); return r; }
    if (od->fs != nd->fs) r = -EXDEV;
    else r = od->ops->rename ? od->ops->rename(od, on, nd, nn) : -EROFS;
    vnode_put(od); vnode_put(nd);
    return r;
}

int vfs_read_all(const char *path, void **data, uint32_t *size) {
    struct vnode *v;
    int r = vfs_lookup("/", path, &v);
    if (r < 0) return r;
    if (!S_ISREG(v->mode)) { vnode_put(v); return -EISDIR; }
    uint32_t n = v->size;
    uint8_t *buf = kmalloc(n + 1);
    if (!buf) { vnode_put(v); return -ENOMEM; }
    uint32_t got = 0;
    r = 0;
    while (got < n) {
        r = v->ops->read(v, got, buf + got, n - got);
        if (r <= 0) break;
        got += r;
    }
    vnode_put(v);
    if (r < 0) { kfree(buf); return r; }
    buf[got] = 0;
    *data = buf; *size = got;
    return 0;
}
