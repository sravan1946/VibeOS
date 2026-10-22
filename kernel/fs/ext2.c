/* ext2.c - ext2 filesystem, read/write.
 *
 * Block cache is a small write-through LRU (copy in/out); inodes are written
 * back eagerly; superblock + group descriptors are flushed at the end of every
 * operation that changed them.  Vnodes are cached per inode in fs->nodes. */
#include "vfs.h"

#define EXT2_MAGIC   0xEF53
#define ROOT_INO     2
#define NDIR         12
#define CACHE_N      1024    /* ponytail: linear LRU scan; hash it if this grows much more */
#define INDEX_FL     0x1000
#define INCOMPAT_FILETYPE 0x2
#define RO_OK        (0x1 | 0x2 | 0x4 | 0x20 | 0x40)  /* sparse_super large_file btree_dir dir_nlink extra_isize */
#define S_IFLNK_     0xA000
#define MAX_LINKS    65000

struct e2_sb {
    uint32_t inodes_count, blocks_count, r_blocks_count, free_blocks, free_inodes;
    uint32_t first_data_block, log_block_size, log_frag, blocks_per_group, frags_per_group, inodes_per_group;
    uint32_t mtime, wtime;
    uint16_t mnt_count, max_mnt, magic, state, errors, minor_rev;
    uint32_t lastcheck, checkinterval, creator_os, rev_level;
    uint16_t def_resuid, def_resgid;
    uint32_t first_ino;
    uint16_t inode_size, block_group_nr;
    uint32_t feat_compat, feat_incompat, feat_ro;
} PACKED;

struct e2_gd {
    uint32_t block_bitmap, inode_bitmap, inode_table;
    uint16_t free_blocks, free_inodes, used_dirs, pad;
    uint32_t resv[3];
} PACKED;

struct e2_inode {                      /* first 128 bytes; the rest of the slot is left alone */
    uint16_t mode, uid;
    uint32_t size, atime, ctime, mtime, dtime;
    uint16_t gid, links;
    uint32_t blocks, flags, osd1, block[15], gen, file_acl, size_high, faddr;
    uint8_t osd2[12];
} PACKED;

struct e2_de {
    uint32_t ino;
    uint16_t rec_len;
    uint8_t name_len, type;
    char name[];
} PACKED;

struct cbuf { uint32_t blk, stamp; uint8_t *data; };

struct e2node;
struct e2fs {
    struct blockdev *dev;
    uint8_t sbraw[1024];
    struct e2_sb *sb;
    struct e2_gd *gd;
    uint32_t gd_blk, gd_nblk;
    uint32_t bs, spb, ppb, ngroups, ipg, bpg, isize, first_ino;
    int ft, dirty;
    struct e2node *nodes;
    struct cbuf cache[CACHE_N];
    uint32_t clock;
    uint8_t *t1, *t2, *ib, *bm, *db, *zero;   /* scratch: indirect, data, inode, bitmap, dir, zeros */
    struct e2fs *next;
};

struct e2node {
    struct vnode v;
    struct e2_inode di;
    struct e2fs *fs;
    uint32_t goal;                     /* allocation hint */
    struct e2node *next;
};

static struct e2fs *mounts;
static const struct vnode_ops e2_ops;

#define NODE(v) ((struct e2node *)(v)->priv)

__attribute__((weak)) uint32_t ext2_now(void) { return 0; }

/* ---- block cache ---- */

static int bio(struct e2fs *fs, uint32_t blk, void *buf, int wr) {
    struct cbuf *c = NULL, *v = fs->cache;
    if (blk < fs->sb->first_data_block || blk == 0 || blk >= fs->sb->blocks_count) return -EIO;
    for (int i = 0; i < CACHE_N; i++) {
        if (fs->cache[i].blk == blk) { c = &fs->cache[i]; break; }
        if (fs->cache[i].stamp < v->stamp) v = &fs->cache[i];
    }
    if (!c) {
        c = v;
        c->blk = 0;
        if (!wr && fs->dev->read(fs->dev, blk * fs->spb, fs->spb, c->data) < 0) return -EIO;
    }
    if (wr) {
        memcpy(c->data, buf, fs->bs);
        c->blk = 0;
        if (fs->dev->write(fs->dev, blk * fs->spb, fs->spb, c->data) < 0) return -EIO;
    } else
        memcpy(buf, c->data, fs->bs);
    c->blk = blk;
    c->stamp = ++fs->clock;
    return 0;
}
static int bread(struct e2fs *fs, uint32_t blk, void *buf) { return bio(fs, blk, buf, 0); }
static int bwrite(struct e2fs *fs, uint32_t blk, const void *buf) { return bio(fs, blk, (void *)buf, 1); }

/* superblock + group descriptors */
static void flush_fs(struct e2fs *fs) {
    if (!fs->dirty) return;
    fs->dirty = 0;
    for (uint32_t i = 0; i < fs->gd_nblk; i++)
        bwrite(fs, fs->gd_blk + i, (uint8_t *)fs->gd + i * fs->bs);
    fs->sb->wtime = ext2_now();
    fs->dev->write(fs->dev, 2, 2, fs->sbraw);
}

/* ---- bitmaps ---- */

static int find_zero(const uint8_t *bm, uint32_t from, uint32_t nbits) {
    for (uint32_t i = 0; i < nbits; i++) {
        uint32_t b = (from + i) % nbits;
        if (!(bm[b >> 3] & (1 << (b & 7)))) return b;
    }
    return -1;
}

/* allocate a zeroed block near goal */
static int alloc_block(struct e2fs *fs, uint32_t goal, uint32_t *out) {
    struct e2_sb *sb = fs->sb;
    if (!sb->free_blocks) return -ENOSPC;
    if (goal < sb->first_data_block || goal >= sb->blocks_count) goal = sb->first_data_block;
    uint32_t g0 = (goal - sb->first_data_block) / fs->bpg;
    for (uint32_t k = 0; k < fs->ngroups; k++) {
        uint32_t g = (g0 + k) % fs->ngroups;
        struct e2_gd *gd = &fs->gd[g];
        if (!gd->free_blocks) continue;
        uint32_t gstart = sb->first_data_block + g * fs->bpg;
        uint32_t nbits = MIN(fs->bpg, sb->blocks_count - gstart);
        int r = bread(fs, gd->block_bitmap, fs->bm);
        if (r < 0) return r;
        int b = find_zero(fs->bm, k ? 0 : goal - gstart, nbits);
        if (b < 0) continue;
        fs->bm[b >> 3] |= 1 << (b & 7);
        if ((r = bwrite(fs, gd->block_bitmap, fs->bm)) < 0) return r;
        if ((r = bwrite(fs, gstart + b, fs->zero)) < 0) return r;
        gd->free_blocks--; sb->free_blocks--; fs->dirty = 1;
        *out = gstart + b;
        return 0;
    }
    return -ENOSPC;
}

static void free_blk(struct e2fs *fs, uint32_t blk) {
    struct e2_sb *sb = fs->sb;
    if (blk < sb->first_data_block || blk >= sb->blocks_count) return;
    uint32_t g = (blk - sb->first_data_block) / fs->bpg, b = (blk - sb->first_data_block) % fs->bpg;
    struct e2_gd *gd = &fs->gd[g];
    if (bread(fs, gd->block_bitmap, fs->bm) < 0 || !(fs->bm[b >> 3] & (1 << (b & 7)))) return;
    fs->bm[b >> 3] &= ~(1 << (b & 7));
    if (bwrite(fs, gd->block_bitmap, fs->bm) < 0) return;
    gd->free_blocks++; sb->free_blocks++; fs->dirty = 1;
}

static int alloc_inode(struct e2fs *fs, uint32_t g0, int isdir, uint32_t *out) {
    if (!fs->sb->free_inodes) return -ENOSPC;
    for (uint32_t k = 0; k < fs->ngroups; k++) {
        uint32_t g = (g0 + k) % fs->ngroups;
        struct e2_gd *gd = &fs->gd[g];
        if (!gd->free_inodes) continue;
        int r = bread(fs, gd->inode_bitmap, fs->bm);
        if (r < 0) return r;
        for (uint32_t b = 0; b < fs->ipg; b++) {
            if (fs->bm[b >> 3] & (1 << (b & 7))) continue;
            uint32_t ino = g * fs->ipg + b + 1;
            if (ino < fs->first_ino) continue;
            fs->bm[b >> 3] |= 1 << (b & 7);
            if ((r = bwrite(fs, gd->inode_bitmap, fs->bm)) < 0) return r;
            gd->free_inodes--; fs->sb->free_inodes--;
            if (isdir) gd->used_dirs++;
            fs->dirty = 1;
            *out = ino;
            return 0;
        }
    }
    return -ENOSPC;
}

static void free_inode(struct e2fs *fs, uint32_t ino, int isdir) {
    uint32_t g = (ino - 1) / fs->ipg, b = (ino - 1) % fs->ipg;
    struct e2_gd *gd = &fs->gd[g];
    if (bread(fs, gd->inode_bitmap, fs->bm) < 0 || !(fs->bm[b >> 3] & (1 << (b & 7)))) return;
    fs->bm[b >> 3] &= ~(1 << (b & 7));
    if (bwrite(fs, gd->inode_bitmap, fs->bm) < 0) return;
    gd->free_inodes++; fs->sb->free_inodes++;
    if (isdir && gd->used_dirs) gd->used_dirs--;
    fs->dirty = 1;
}

/* ---- inodes ---- */

static int inode_loc(struct e2fs *fs, uint32_t ino, uint32_t *blk, uint32_t *off) {
    if (ino == 0 || ino > fs->sb->inodes_count) return -EIO;
    uint32_t idx = (ino - 1) % fs->ipg, byte = idx * fs->isize;
    *blk = fs->gd[(ino - 1) / fs->ipg].inode_table + byte / fs->bs;
    *off = byte % fs->bs;
    return 0;
}

static int read_inode(struct e2fs *fs, uint32_t ino, struct e2_inode *di) {
    uint32_t blk, off;
    int r = inode_loc(fs, ino, &blk, &off);
    if (r < 0 || (r = bread(fs, blk, fs->ib)) < 0) return r;
    memcpy(di, fs->ib + off, sizeof *di);
    return 0;
}

static int write_inode(struct e2fs *fs, uint32_t ino, const struct e2_inode *di) {
    uint32_t blk, off;
    int r = inode_loc(fs, ino, &blk, &off);
    if (r < 0 || (r = bread(fs, blk, fs->ib)) < 0) return r;
    memcpy(fs->ib + off, di, sizeof *di);
    return bwrite(fs, blk, fs->ib);
}

static void v_sync(struct e2node *n) {
    struct vnode *v = &n->v;
    v->mode = n->di.mode; v->size = n->di.size; v->nlink = n->di.links;
    v->mtime = n->di.mtime; v->ctime = n->di.ctime; v->atime = n->di.atime;
}

static int node_flush(struct e2node *n) {
    v_sync(n);
    return write_inode(n->fs, n->v.ino, &n->di);
}

static void touch(struct e2node *n) { n->di.mtime = n->di.ctime = ext2_now(); }

static int iget(struct e2fs *fs, uint32_t ino, struct e2node **out) {
    struct e2node *n;
    for (n = fs->nodes; n; n = n->next)
        if (n->v.ino == ino) { n->v.refs++; *out = n; return 0; }
    if (!(n = kzalloc(sizeof *n))) return -ENOMEM;
    int r = read_inode(fs, ino, &n->di);
    if (r < 0 || !n->di.mode) { kfree(n); return r < 0 ? r : -EIO; }
    n->fs = fs;
    n->goal = fs->sb->first_data_block + ((ino - 1) / fs->ipg) * fs->bpg;
    n->v.ino = ino; n->v.refs = 1; n->v.ops = &e2_ops; n->v.fs = fs; n->v.priv = n;
    v_sync(n);
    n->next = fs->nodes; fs->nodes = n;
    *out = n;
    return 0;
}

static void e2_release(struct vnode *v);
static void put(struct e2node *n) { if (--n->v.refs == 0) e2_release(&n->v); }

/* ---- block mapping ---- */

/* entry idx of the inode's i_block (tb == 0) or of indirect block tb */
static int slot(struct e2node *n, uint32_t tb, uint32_t idx, int alloc, uint32_t *out) {
    struct e2fs *fs = n->fs;
    uint32_t *tab = NULL, v, nb;
    int r;
    if (!tb) v = n->di.block[idx];
    else {
        if ((r = bread(fs, tb, fs->t1)) < 0) return r;
        tab = (uint32_t *)fs->t1;
        v = tab[idx];
    }
    if (!v && alloc) {
        if ((r = alloc_block(fs, n->goal, &nb)) < 0) return r;
        if (tab) {
            tab[idx] = nb;
            if ((r = bwrite(fs, tb, tab)) < 0) { free_blk(fs, nb); return r; }
        } else
            n->di.block[idx] = nb;
        n->di.blocks += fs->spb;
        n->goal = nb + 1;
        v = nb;
    } else if (v && (v < fs->sb->first_data_block || v >= fs->sb->blocks_count))
        return -EIO;
    *out = v;
    return 0;
}

/* logical -> physical block; *out = 0 for a hole when !alloc */
static int bmap(struct e2node *n, uint32_t lb, int alloc, uint32_t *out) {
    uint32_t P = n->fs->ppb, path[4], tb = 0;
    int depth;
    if (lb < NDIR) { path[0] = lb; depth = 1; }
    else if ((lb -= NDIR) < P) { path[0] = 12; path[1] = lb; depth = 2; }
    else if ((lb -= P) < P * P) { path[0] = 13; path[1] = lb / P; path[2] = lb % P; depth = 3; }
    else { lb -= P * P; path[0] = 14; path[1] = lb / (P * P); path[2] = (lb / P) % P; path[3] = lb % P; depth = 4; }
    for (int k = 0; k < depth; k++) {
        int r = slot(n, tb, path[k], alloc, &tb);
        if (r < 0) return r;
        if (!tb) break;
    }
    *out = tb;
    return 0;
}

/* free the subtree under indirect block blk (lvl >= 1) from logical entry `from`;
 * returns 1 if blk itself was freed */
static int free_sub(struct e2node *n, uint32_t blk, int lvl, uint32_t from) {
    struct e2fs *fs = n->fs;
    if (!lvl) { free_blk(fs, blk); n->di.blocks -= MIN(n->di.blocks, fs->spb); return 1; }
    if (blk < fs->sb->first_data_block || blk >= fs->sb->blocks_count) return 0;
    uint32_t *t = kmalloc(fs->bs), span = 1, i;
    if (!t) return 0;
    if (bread(fs, blk, t) < 0) { kfree(t); return 0; }
    for (i = 1; i < (uint32_t)lvl; i++) span *= fs->ppb;
    int ch = 0, empty = 1;
    for (i = 0; i < fs->ppb; i++) {
        if (!t[i]) continue;
        uint32_t cs = i * span;
        if (from < cs + span && free_sub(n, t[i], lvl - 1, from > cs ? from - cs : 0)) { t[i] = 0; ch = 1; }
        else empty = 0;
    }
    if (ch && !empty) bwrite(fs, blk, t);
    kfree(t);
    if (!empty) return 0;
    free_blk(fs, blk);
    n->di.blocks -= MIN(n->di.blocks, fs->spb);
    return 1;
}

/* free every block at logical index >= first */
static void trunc_from(struct e2node *n, uint32_t first) {
    struct e2fs *fs = n->fs;
    uint32_t base = NDIR, span = fs->ppb;
    for (uint32_t i = first; i < NDIR; i++)
        if (n->di.block[i]) { free_blk(fs, n->di.block[i]); n->di.blocks -= MIN(n->di.blocks, fs->spb); n->di.block[i] = 0; }
    for (int k = 12; k < 15; k++) {
        if (n->di.block[k] && first < base + span &&
            free_sub(n, n->di.block[k], k - 11, first > base ? first - base : 0))
            n->di.block[k] = 0;
        base += span; span *= fs->ppb;
    }
}

/* ---- file data ---- */

static int e2_read(struct vnode *v, uint32_t off, void *buf, uint32_t len) {
    struct e2node *n = NODE(v);
    struct e2fs *fs = n->fs;
    uint8_t *dst = buf;
    uint32_t done = 0;
    if (S_ISDIR(n->di.mode)) return -EISDIR;
    if (off >= n->di.size) return 0;
    len = MIN(len, n->di.size - off);
    while (done < len) {
        uint32_t pos = off + done, bo = pos % fs->bs, chunk = MIN(fs->bs - bo, len - done), pb;
        int r = bmap(n, pos / fs->bs, 0, &pb);
        if (r >= 0) {
            if (!pb) memset(dst + done, 0, chunk);
            else if (chunk == fs->bs) r = bread(fs, pb, dst + done);
            else if ((r = bread(fs, pb, fs->t2)) >= 0) memcpy(dst + done, fs->t2 + bo, chunk);
        }
        if (r < 0) return done ? (int)done : r;
        done += chunk;
    }
    return done;
}

static int e2_write(struct vnode *v, uint32_t off, const void *buf, uint32_t len) {
    struct e2node *n = NODE(v);
    struct e2fs *fs = n->fs;
    const uint8_t *src = buf;
    uint32_t done = 0;
    int err = 0;
    if (!S_ISREG(n->di.mode)) return S_ISDIR(n->di.mode) ? -EISDIR : -EINVAL;
    if (!len) return 0;
    if (off == 0xFFFFFFFF) return -EFBIG;
    len = MIN(len, 0xFFFFFFFF - off);               /* sizes are 32-bit */
    while (done < len) {
        uint32_t pos = off + done, bo = pos % fs->bs, chunk = MIN(fs->bs - bo, len - done), pb;
        int r = bmap(n, pos / fs->bs, 1, &pb);
        if (r >= 0) {
            if (chunk == fs->bs) r = bwrite(fs, pb, src + done);
            else if ((r = bread(fs, pb, fs->t2)) >= 0) {
                memcpy(fs->t2 + bo, src + done, chunk);
                r = bwrite(fs, pb, fs->t2);
            }
        }
        if (r < 0) { err = r; break; }
        done += chunk;
    }
    if (done && off + done > n->di.size) n->di.size = off + done;
    if (done) touch(n);
    int r = node_flush(n);
    flush_fs(fs);
    if (!done) return err ? err : r;
    return done;
}

static int e2_truncate(struct vnode *v, uint32_t size) {
    struct e2node *n = NODE(v);
    struct e2fs *fs = n->fs;
    if (S_ISDIR(n->di.mode)) return -EISDIR;
    if (!S_ISREG(n->di.mode)) return -EINVAL;
    if (size < n->di.size) {
        uint32_t bo = size % fs->bs, pb;
        if (bo && bmap(n, size / fs->bs, 0, &pb) >= 0 && pb && bread(fs, pb, fs->t2) >= 0) {
            memset(fs->t2 + bo, 0, fs->bs - bo);    /* regrowing must read zeros */
            bwrite(fs, pb, fs->t2);
        }
        trunc_from(n, size / fs->bs + (bo ? 1 : 0));
    }
    n->di.size = size;
    touch(n);
    int r = node_flush(n);
    flush_fs(fs);
    return r;
}

/* ---- directories ---- */

static int check_name(const char *s) {
    size_t l = strlen(s);
    if (!l || strchr(s, '/')) return -EINVAL;
    return l > NAME_MAX ? -ENAMETOOLONG : (int)l;
}
static int is_dots(const char *s) { return !strcmp(s, ".") || !strcmp(s, ".."); }

static uint8_t ftype(struct e2fs *fs, uint32_t mode) {
    if (!fs->ft) return 0;
    switch (mode & S_IFMT) {
    case S_IFREG: return 1;
    case S_IFDIR: return 2;
    case S_IFCHR: return 3;
    case S_IFIFO: return 5;
    }
    return 0;
}

/* validate the entry at buf+off; returns rec_len or -EIO */
static int de_ok(struct e2fs *fs, const uint8_t *buf, uint32_t off) {
    const struct e2_de *de = (const struct e2_de *)(buf + off);
    uint32_t rl = de->rec_len;
    if (off + 8 > fs->bs || rl < 8 || (rl & 3) || off + rl > fs->bs || 8u + de->name_len > rl) return -EIO;
    return rl;
}

static int dir_blk(struct e2node *d, uint32_t lb, uint32_t *pb, void *buf) {
    int r = bmap(d, lb, 0, pb);
    if (r < 0) return r;
    return *pb ? bread(d->fs, *pb, buf) : -EIO;
}

/* find name; *prev = offset of the previous entry in the same block or -1 */
static int dir_find(struct e2node *d, const char *name, uint32_t nlen, uint32_t *ino,
                    uint32_t *lb, uint32_t *off, uint32_t *prev) {
    struct e2fs *fs = d->fs;
    uint32_t nb = d->di.size / fs->bs, pb;
    for (uint32_t b = 0; b < nb; b++) {
        int r = dir_blk(d, b, &pb, fs->db);
        if (r < 0) return r;
        uint32_t p = -1;
        for (uint32_t o = 0, rl; o < fs->bs; p = o, o += rl) {
            if ((r = de_ok(fs, fs->db, o)) < 0) return r;
            rl = r;
            struct e2_de *de = (struct e2_de *)(fs->db + o);
            if (de->ino && de->name_len == nlen && !memcmp(de->name, name, nlen)) {
                *ino = de->ino; *lb = b; *off = o; *prev = p;
                return 0;
            }
        }
    }
    return -ENOENT;
}

static void de_fill(struct e2_de *de, uint32_t ino, uint8_t type, const char *name, uint32_t nlen) {
    de->ino = ino; de->name_len = nlen; de->type = type;
    memcpy(de->name, name, nlen);
}

static int dir_add(struct e2node *d, const char *name, uint32_t nlen, uint32_t ino, uint8_t type) {
    struct e2fs *fs = d->fs;
    uint32_t need = ALIGN_UP(8 + nlen, 4), nb = d->di.size / fs->bs, pb;
    for (uint32_t b = 0; b < nb; b++) {
        int r = dir_blk(d, b, &pb, fs->db);
        if (r < 0) return r;
        for (uint32_t o = 0, rl; o < fs->bs; o += rl) {
            if ((r = de_ok(fs, fs->db, o)) < 0) return r;
            rl = r;
            struct e2_de *de = (struct e2_de *)(fs->db + o);
            uint32_t used = de->ino ? ALIGN_UP(8 + de->name_len, 4) : 0;
            if (rl - used < need) continue;
            if (used) {                              /* split the slack */
                struct e2_de *ne = (struct e2_de *)(fs->db + o + used);
                ne->rec_len = rl - used;
                de->rec_len = used;
                de = ne;
            }
            de_fill(de, ino, type, name, nlen);
            d->di.flags &= ~INDEX_FL;
            return bwrite(fs, pb, fs->db);
        }
    }
    int r = bmap(d, nb, 1, &pb);                     /* directory full: add a block */
    if (r < 0) return r;
    memset(fs->db, 0, fs->bs);
    struct e2_de *de = (struct e2_de *)fs->db;
    de->rec_len = fs->bs;
    de_fill(de, ino, type, name, nlen);
    if ((r = bwrite(fs, pb, fs->db)) < 0) return r;
    d->di.size += fs->bs;
    d->di.flags &= ~INDEX_FL;
    return 0;
}

/* drop the entry at (lb, off); merge it into prev if there is one */
static int dir_remove(struct e2node *d, uint32_t lb, uint32_t off, uint32_t prev) {
    struct e2fs *fs = d->fs;
    uint32_t pb;
    int r = dir_blk(d, lb, &pb, fs->db);
    if (r < 0) return r;
    struct e2_de *de = (struct e2_de *)(fs->db + off);
    if (prev != (uint32_t)-1) ((struct e2_de *)(fs->db + prev))->rec_len += de->rec_len;
    else de->ino = 0;
    d->di.flags &= ~INDEX_FL;
    return bwrite(fs, pb, fs->db);
}

static int dir_setino(struct e2node *d, uint32_t lb, uint32_t off, uint32_t ino, uint8_t type) {
    struct e2fs *fs = d->fs;
    uint32_t pb;
    int r = dir_blk(d, lb, &pb, fs->db);
    if (r < 0) return r;
    struct e2_de *de = (struct e2_de *)(fs->db + off);
    de->ino = ino;
    if (fs->ft) de->type = type;
    return bwrite(fs, pb, fs->db);
}

/* 1 = only . and .. */
static int dir_empty(struct e2node *d) {
    struct e2fs *fs = d->fs;
    uint32_t nb = d->di.size / fs->bs, pb;
    for (uint32_t b = 0; b < nb; b++) {
        int r = dir_blk(d, b, &pb, fs->db);
        if (r < 0) return r;
        for (uint32_t o = 0, rl; o < fs->bs; o += rl) {
            if ((r = de_ok(fs, fs->db, o)) < 0) return r;
            rl = r;
            struct e2_de *de = (struct e2_de *)(fs->db + o);
            if (de->ino && !((de->name_len == 1 && de->name[0] == '.') ||
                             (de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.')))
                return 0;
        }
    }
    return 1;
}

static int e2_readdir(struct vnode *dv, uint32_t *cookie, struct dirent *out) {
    struct e2node *d = NODE(dv);
    struct e2fs *fs = d->fs;
    if (!S_ISDIR(d->di.mode)) return -ENOTDIR;
    for (;;) {
        uint32_t c = *cookie, lb = c / fs->bs, bo = c % fs->bs, pb, o, rl = 0;
        if (c >= d->di.size) return 0;
        int r = dir_blk(d, lb, &pb, fs->db);
        if (r < 0) return r;
        for (o = 0; o < fs->bs; o += rl) {          /* resync to an entry boundary */
            if ((r = de_ok(fs, fs->db, o)) < 0) return r;
            rl = r;
            if (o >= bo) break;
        }
        if (o >= fs->bs) { *cookie = (lb + 1) * fs->bs; continue; }
        *cookie = lb * fs->bs + o + rl;
        struct e2_de *de = (struct e2_de *)(fs->db + o);
        if (!de->ino) continue;
        out->d_ino = de->ino;
        out->d_type = DT_UNKNOWN;
        switch (de->type) {
        case 1: out->d_type = DT_REG; break;
        case 2: out->d_type = DT_DIR; break;
        case 3: out->d_type = DT_CHR; break;
        case 5: out->d_type = DT_FIFO; break;
        }
        memcpy(out->d_name, de->name, de->name_len);
        out->d_name[de->name_len] = 0;
        if (!de->type) {                            /* no filetype feature: ask the inode */
            struct e2_inode di;
            if (read_inode(fs, out->d_ino, &di) >= 0)
                out->d_type = S_ISDIR(di.mode) ? DT_DIR : S_ISREG(di.mode) ? DT_REG :
                              S_ISCHR(di.mode) ? DT_CHR : DT_UNKNOWN;
        }
        return 1;
    }
}

static int e2_lookup(struct vnode *dv, const char *name, struct vnode **out) {
    struct e2node *d = NODE(dv), *n;
    uint32_t ino, lb, off, prev;
    if (!S_ISDIR(d->di.mode)) return -ENOTDIR;
    int l = check_name(name);
    if (l < 0) return l;
    int r = dir_find(d, name, l, &ino, &lb, &off, &prev);
    if (r < 0) return r;
    if ((r = iget(d->fs, ino, &n)) < 0) return r;
    *out = &n->v;
    return 0;
}

static int e2_create(struct vnode *dv, const char *name, uint32_t mode, struct vnode **out) {
    struct e2node *d = NODE(dv), *n;
    struct e2fs *fs = d->fs;
    uint32_t ino, lb, off, prev, type = mode & S_IFMT, blk = 0;
    if (!S_ISDIR(d->di.mode)) return -ENOTDIR;
    if (type != S_IFREG && type != S_IFDIR) return -EINVAL;
    int l = check_name(name);
    if (l < 0) return l;
    int r = dir_find(d, name, l, &ino, &lb, &off, &prev);
    if (!r) return -EEXIST;
    if (r != -ENOENT) return r;
    int isdir = type == S_IFDIR;
    if (isdir && d->di.links >= MAX_LINKS) return -ENOSPC;
    if ((r = alloc_inode(fs, (d->v.ino - 1) / fs->ipg, isdir, &ino)) < 0) return r;
    struct e2_inode di;
    memset(&di, 0, sizeof di);
    di.mode = type | (mode & 07777);
    di.links = isdir ? 2 : 1;
    di.atime = di.ctime = di.mtime = ext2_now();
    if (isdir) {
        if ((r = alloc_block(fs, d->goal, &blk)) < 0) goto fail;
        memset(fs->db, 0, fs->bs);
        struct e2_de *de = (struct e2_de *)fs->db;
        de_fill(de, ino, ftype(fs, S_IFDIR), ".", 1);
        de->rec_len = 12;
        de = (struct e2_de *)(fs->db + 12);
        de_fill(de, d->v.ino, ftype(fs, S_IFDIR), "..", 2);
        de->rec_len = fs->bs - 12;
        if ((r = bwrite(fs, blk, fs->db)) < 0) goto fail;
        di.block[0] = blk; di.size = fs->bs; di.blocks = fs->spb;
    }
    if ((r = write_inode(fs, ino, &di)) < 0) goto fail;
    if ((r = dir_add(d, name, l, ino, ftype(fs, type))) < 0) goto fail;
    if (isdir) d->di.links++;
    touch(d);
    node_flush(d);
    if ((r = iget(fs, ino, &n)) >= 0) *out = &n->v;
    flush_fs(fs);
    return r;
fail:
    if (blk) free_blk(fs, blk);
    free_inode(fs, ino, isdir);
    flush_fs(fs);
    return r;
}

static int e2_unlink(struct vnode *dv, const char *name, bool is_dir) {
    struct e2node *d = NODE(dv), *t;
    struct e2fs *fs = d->fs;
    uint32_t ino, lb, off, prev;
    if (!S_ISDIR(d->di.mode)) return -ENOTDIR;
    int l = check_name(name);
    if (l < 0) return l;
    if (is_dots(name)) return -EINVAL;
    int r = dir_find(d, name, l, &ino, &lb, &off, &prev);
    if (r < 0) return r;
    if ((r = iget(fs, ino, &t)) < 0) return r;
    int isdir = S_ISDIR(t->di.mode);
    if (isdir && !is_dir) r = -EISDIR;
    else if (!isdir && is_dir) r = -ENOTDIR;
    else if (isdir && (r = dir_empty(t)) >= 0) r = r ? 0 : -ENOTEMPTY;
    if (r < 0) { put(t); return r; }
    if ((r = dir_remove(d, lb, off, prev)) < 0) { put(t); return r; }
    if (isdir) { t->di.links = 0; if (d->di.links > 2) d->di.links--; }
    else if (t->di.links) t->di.links--;
    touch(d); t->di.ctime = ext2_now();
    node_flush(d);
    node_flush(t);
    put(t);                                         /* last ref frees blocks + inode */
    flush_fs(fs);
    return 0;
}

/* is dir `target` an ancestor of (or equal to) dir `ino`? */
static int is_ancestor(struct e2fs *fs, uint32_t target, uint32_t ino) {
    for (int i = 0; i < 4096; i++) {
        struct e2node *t;
        uint32_t p, lb, off, prev;
        if (ino == target) return 1;
        if (ino == ROOT_INO) return 0;
        if (iget(fs, ino, &t) < 0) return 0;
        int r = dir_find(t, "..", 2, &p, &lb, &off, &prev);
        put(t);
        if (r < 0) return 0;
        ino = p;
    }
    return 0;
}

static int e2_rename(struct vnode *odv, const char *oname, struct vnode *ndv, const char *nname) {
    struct e2node *od = NODE(odv), *nd = NODE(ndv), *sn, *tn;
    struct e2fs *fs = od->fs;
    uint32_t sino, tino, lb, off, prev, tlb, toff, tprev;
    if (nd->fs != fs) return -EXDEV;
    if (!S_ISDIR(od->di.mode) || !S_ISDIR(nd->di.mode)) return -ENOTDIR;
    int ol = check_name(oname), nl = check_name(nname);
    if (ol < 0) return ol;
    if (nl < 0) return nl;
    if (is_dots(oname) || is_dots(nname)) return -EINVAL;
    int r = dir_find(od, oname, ol, &sino, &lb, &off, &prev);
    if (r < 0) return r;
    if (od == nd && !strcmp(oname, nname)) return 0;
    if ((r = iget(fs, sino, &sn)) < 0) return r;
    int sdir = S_ISDIR(sn->di.mode), moved = sdir && od != nd;
    uint8_t st = ftype(fs, sn->di.mode);
    if (moved && is_ancestor(fs, sino, nd->v.ino)) { put(sn); return -EINVAL; }
    if (moved && nd->di.links >= MAX_LINKS) { put(sn); return -ENOSPC; }

    r = dir_find(nd, nname, nl, &tino, &tlb, &toff, &tprev);
    if (!r) {                                       /* replace an existing target */
        if (tino == sino) { put(sn); return 0; }
        if ((r = iget(fs, tino, &tn)) < 0) { put(sn); return r; }
        int tdir = S_ISDIR(tn->di.mode);
        if (sdir && !tdir) r = -ENOTDIR;
        else if (!sdir && tdir) r = -EISDIR;
        else if (tdir && (r = dir_empty(tn)) >= 0) r = r ? 0 : -ENOTEMPTY;
        if (r < 0 || (r = dir_setino(nd, tlb, toff, sino, st)) < 0) { put(tn); put(sn); return r; }
        if (tdir) { tn->di.links = 0; if (nd->di.links > 2) nd->di.links--; }
        else if (tn->di.links) tn->di.links--;
        tn->di.ctime = ext2_now();
        node_flush(tn);
        put(tn);
    } else if (r == -ENOENT) {
        if ((r = dir_add(nd, nname, nl, sino, st)) < 0) { put(sn); return r; }
    } else { put(sn); return r; }

    /* offsets may have moved if od == nd */
    if ((r = dir_find(od, oname, ol, &sino, &lb, &off, &prev)) >= 0)
        r = dir_remove(od, lb, off, prev);
    if (r >= 0 && moved) {
        if (dir_find(sn, "..", 2, &tino, &lb, &off, &prev) >= 0)
            dir_setino(sn, lb, off, nd->v.ino, ftype(fs, S_IFDIR));
        if (od->di.links > 2) od->di.links--;
        nd->di.links++;
    }
    touch(od); touch(nd);
    sn->di.ctime = ext2_now();
    node_flush(od); if (nd != od) node_flush(nd);
    node_flush(sn);
    put(sn);
    flush_fs(fs);
    return r < 0 ? r : 0;
}

static int e2_sync(struct vnode *v) {
    struct e2node *n = NODE(v);
    int r = node_flush(n);
    n->fs->dirty = 1;
    flush_fs(n->fs);
    return r;
}

/* last reference gone: free the file if it was unlinked */
static void e2_release(struct vnode *v) {
    struct e2node *n = NODE(v), **pp;
    struct e2fs *fs = n->fs;
    for (pp = &fs->nodes; *pp; pp = &(*pp)->next)
        if (*pp == n) { *pp = n->next; break; }
    if (!n->di.links) {
        uint32_t t = n->di.mode & S_IFMT;
        if (t == S_IFREG || t == S_IFDIR || (t == S_IFLNK_ && n->di.blocks)) trunc_from(n, 0);
        n->di.size = 0;
        n->di.dtime = ext2_now();
        write_inode(fs, v->ino, &n->di);
        free_inode(fs, v->ino, t == S_IFDIR);
    }
    kfree(n);
    flush_fs(fs);
}

static const struct vnode_ops e2_ops = {
    .read = e2_read, .write = e2_write, .readdir = e2_readdir, .lookup = e2_lookup,
    .create = e2_create, .unlink = e2_unlink, .rename = e2_rename, .truncate = e2_truncate,
    .release = e2_release, .sync = e2_sync,
};

/* ---- mount ---- */

int ext2_mount(struct blockdev *dev, struct vnode **root) {
    struct e2fs *fs = kzalloc(sizeof *fs);
    struct e2node *rn;
    int r = -ENOMEM;
    if (!fs) return r;
    fs->dev = dev;
    if (dev->read(dev, 2, 2, fs->sbraw) < 0) { r = -EIO; goto fail; }
    struct e2_sb *sb = fs->sb = (struct e2_sb *)fs->sbraw;
    r = -EINVAL;
    if (sb->magic != EXT2_MAGIC || sb->log_block_size > 2) goto fail;
    fs->bs = 1024u << sb->log_block_size;
    fs->spb = fs->bs / 512;
    fs->ppb = fs->bs / 4;
    fs->bpg = sb->blocks_per_group;
    fs->ipg = sb->inodes_per_group;
    fs->isize = sb->rev_level ? sb->inode_size : 128;
    fs->first_ino = sb->rev_level ? sb->first_ino : 11;
    fs->ft = sb->rev_level && (sb->feat_incompat & INCOMPAT_FILETYPE);
    if (sb->rev_level && ((sb->feat_incompat & ~INCOMPAT_FILETYPE) || (sb->feat_ro & ~RO_OK))) goto fail;
    if (!fs->bpg || !fs->ipg || fs->bpg > 8 * fs->bs || fs->ipg > 8 * fs->bs) goto fail;
    if (fs->isize < 128 || fs->isize > fs->bs || (fs->isize & (fs->isize - 1))) goto fail;
    if (sb->first_data_block != (fs->bs == 1024) || sb->blocks_count <= sb->first_data_block) goto fail;
    if (sb->blocks_count > dev->sectors / fs->spb) goto fail;
    fs->ngroups = (sb->blocks_count - sb->first_data_block + fs->bpg - 1) / fs->bpg;
    if (sb->inodes_count != fs->ngroups * fs->ipg || fs->first_ino < 3) goto fail;
    fs->gd_blk = sb->first_data_block + 1;
    fs->gd_nblk = (fs->ngroups * sizeof(struct e2_gd) + fs->bs - 1) / fs->bs;

    r = -ENOMEM;
    uint8_t *mem = kmalloc((CACHE_N + 6) * fs->bs);
    fs->gd = kmalloc(fs->gd_nblk * fs->bs);
    if (!mem || !fs->gd) { kfree(mem); goto fail; }
    for (int i = 0; i < CACHE_N; i++) fs->cache[i].data = mem + i * fs->bs;
    fs->t1 = mem + CACHE_N * fs->bs;
    fs->t2 = fs->t1 + fs->bs; fs->ib = fs->t2 + fs->bs; fs->bm = fs->ib + fs->bs;
    fs->db = fs->bm + fs->bs; fs->zero = fs->db + fs->bs;
    memset(fs->zero, 0, fs->bs);

    for (uint32_t i = 0; i < fs->gd_nblk; i++)
        if ((r = bread(fs, fs->gd_blk + i, (uint8_t *)fs->gd + i * fs->bs)) < 0) goto fail2;
    r = -EINVAL;
    uint32_t itb = (fs->ipg * fs->isize + fs->bs - 1) / fs->bs;
    for (uint32_t g = 0; g < fs->ngroups; g++) {
        struct e2_gd *gd = &fs->gd[g];
        if (gd->block_bitmap < sb->first_data_block || gd->block_bitmap >= sb->blocks_count ||
            gd->inode_bitmap < sb->first_data_block || gd->inode_bitmap >= sb->blocks_count ||
            gd->inode_table < sb->first_data_block || gd->inode_table + itb > sb->blocks_count)
            goto fail2;
    }
    if ((r = iget(fs, ROOT_INO, &rn)) < 0) goto fail2;
    if (!S_ISDIR(rn->di.mode)) { put(rn); r = -EINVAL; goto fail2; }
    fs->next = mounts; mounts = fs;
    *root = &rn->v;
    return 0;
fail2:
    kfree(fs->cache[0].data);
    kfree(fs->gd);
fail:
    kfree(fs);
    return r;
}

int ext2_sync_all(void) {
    for (struct e2fs *fs = mounts; fs; fs = fs->next) {
        for (struct e2node *n = fs->nodes; n; n = n->next) node_flush(n);
        fs->dirty = 1;
        flush_fs(fs);
    }
    return 0;
}
