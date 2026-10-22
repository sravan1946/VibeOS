/* vfs.h - virtual filesystem layer.
 *
 * Paths are resolved lexically (vfs_canon) and then walked from the root of
 * the longest matching mount.  Filesystems implement vnode_ops; every
 * function returns >= 0 on success or -errno (see abi.h).
 *
 * Reference counting: a vnode returned by lookup/create/vfs_open_vnode is
 * owned by the caller, who must vnode_put() it.  Filesystems may cache vnodes
 * (same inode -> same vnode) and must free them in ops->release, which is
 * called when refs drops to 0. */
#pragma once
#include "kernel.h"
#include "abi.h"

struct vnode;

struct vnode_ops {
    int  (*read)(struct vnode *v, uint32_t off, void *buf, uint32_t n);         /* bytes read, 0 at EOF */
    int  (*write)(struct vnode *v, uint32_t off, const void *buf, uint32_t n);  /* bytes written */
    int  (*readdir)(struct vnode *dir, uint32_t *cookie, struct dirent *out);   /* 1 = entry, 0 = end; cookie starts 0 */
    int  (*lookup)(struct vnode *dir, const char *name, struct vnode **out);    /* -ENOENT if missing */
    int  (*create)(struct vnode *dir, const char *name, uint32_t mode, struct vnode **out); /* mode: S_IFREG|0644 or S_IFDIR|0755; -EEXIST */
    int  (*unlink)(struct vnode *dir, const char *name, bool is_dir);           /* is_dir: rmdir semantics (-ENOTEMPTY) */
    int  (*rename)(struct vnode *odir, const char *oname, struct vnode *ndir, const char *nname);
    int  (*truncate)(struct vnode *v, uint32_t size);
    int  (*ioctl)(struct vnode *v, int req, void *arg);
    int  (*poll)(struct vnode *v, int events);                                  /* ready POLL* bits now */
    void (*close)(struct vnode *v, int flags);                                  /* an open file on v closed */
    void (*release)(struct vnode *v);                                           /* refs hit 0 */
    int  (*sync)(struct vnode *v);
};

struct vnode {
    uint32_t mode;            /* S_IF* | perms */
    uint32_t size;
    uint32_t ino;
    uint32_t nlink;
    uint32_t mtime, ctime, atime;
    int refs;
    const struct vnode_ops *ops;
    void *fs;                 /* filesystem instance */
    void *priv;               /* per-node data */
};

static inline void vnode_get(struct vnode *v) { v->refs++; }
void vnode_put(struct vnode *v);

/* an open file description (shared by dup'ed fds) */
struct file {
    struct vnode *vn;
    uint32_t off;
    int flags;                /* O_* */
    int refs;
    uint32_t dir_cookie;
};

/* block devices (dev/ata.c) */
struct blockdev {
    char name[8];
    uint32_t sectors;         /* 512-byte sectors */
    int (*read)(struct blockdev *d, uint32_t lba, uint32_t count, void *buf);
    int (*write)(struct blockdev *d, uint32_t lba, uint32_t count, const void *buf);
    void *priv;
};
struct blockdev *blockdev_get(int index);           /* NULL if none */
/* a partition view on top of a disk; reads beyond len fail */
struct blockdev *blockdev_partition(struct blockdev *disk, uint32_t start, uint32_t len);
void ata_flush_all(void);                            /* write caches -> disk, before power-off */
int  clipboard_get(char *buf, int n);                 /* /dev/clipboard contents, for kernel windows */
void clipboard_set(const char *s, int n);
int ata_init(void);                                  /* probe IDE channels; returns disks found */
struct blockdev *blockdev_root_partition(void);      /* first type-0x83 partition of disk 0, NULL if none */

/* filesystems */
int ext2_mount(struct blockdev *dev, struct vnode **root);   /* fs/ext2.c */
int ext2_sync_all(void);
int vfs_mount(const char *path, struct vnode *root);         /* path must be canonical, e.g. "/dev" */

/* path API (fs/vfs.c), `cwd` is a canonical absolute path */
int  vfs_canon(const char *cwd, const char *path, char *out, size_t n);
int  vfs_lookup(const char *cwd, const char *path, struct vnode **out);
int  vfs_open(const char *cwd, const char *path, int flags, struct file **out);
struct file *vfs_open_vnode(struct vnode *v, int flags);     /* takes a ref on v */
void vfs_close(struct file *f);
int  vfs_read(struct file *f, void *buf, uint32_t n);
int  vfs_write(struct file *f, const void *buf, uint32_t n);
int  vfs_seek(struct file *f, int32_t off, int whence);
int  vfs_stat_vnode(struct vnode *v, struct stat *st);
int  vfs_mkdir(const char *cwd, const char *path);
int  vfs_unlink(const char *cwd, const char *path, bool is_dir);
int  vfs_rename(const char *cwd, const char *oldp, const char *newp);
int  vfs_read_all(const char *path, void **data, uint32_t *size);   /* kmalloc'd buffer */

/* dev/ac97.c: /dev/dsp (write 48 kHz s16le stereo) */
int ac97_init(void);                                         /* 0 or -ENODEV; devfs then exposes dsp */
extern const struct vnode_ops ac97_ops;
int ac97_volume(int set);                                   /* master volume (set < 0 just reads); -1 without a card */
