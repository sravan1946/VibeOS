/* malloc.c - boundary-tag allocator over sbrk with segregated free lists.
 *
 * Every block starts with an 8-byte header { size | PREV_FREE, magic }. A free block also carries
 * next/prev links (in the payload) and a footer repeating its size, so free() merges with both
 * physical neighbours in O(1). Free blocks live in bins: one per 8-byte size up to 504 bytes, then
 * one per power of two. Heap memory is never returned to the kernel, but blocks of BIG bytes or more
 * get their own mmap (header { mapping size, MAGIC_MMAP }) and free() unmaps them.
 * The heap ends in a zero-size "used" sentinel so merging never runs off the end.
 *
 * Fast path: a small block (<= FAST_MAX) that is freed goes on a per-size stack without merging (it still
 * counts as used: neighbours don't see a free block), and the next malloc of that size pops it. The stacks are
 * merged back into the bins when the heap would otherwise grow or they hold more than FAST_KEEP bytes.
 * `binmap` has a bit per non-empty bin so a miss in the exact bin doesn't walk ~90 empty ones. */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <sys/mman.h>

#define HDR 8
#define MINBLK 24                       /* header + next + prev + footer */
#define PREV_FREE 1u
#define MAGIC_USED 0xA110C8EDu
#define MAGIC_FREE 0xF4EEB10Cu
#define MAGIC_MMAP 0x3A9B10C5u
#define BIG (128 * 1024)
#define CHUNK 65536
#define NBINS 96
#define FAST_MAX 128                    /* block sizes up to this are cached by exact size */
#define FAST_KEEP (64 * 1024)
#define MAGIC_FAST 0xFA57B10Cu

struct blk {
    uint32_t size;                      /* multiple of 8; bit 0 = the block before this one is free */
    uint32_t magic;
    struct blk *next, *prev;            /* free blocks only; overlap user data */
};

static struct blk *bins[NBINS];
static uint32_t binmap[NBINS / 32];
static struct blk *fast[FAST_MAX / 8 + 1];     /* by block size / 8 */
static size_t fast_bytes;
static char *heap_end;                  /* just past the sentinel's header */

#define SZ(b) ((b)->size & ~7u)
#define NEXT(b) ((struct blk *)((char *)(b) + SZ(b)))
#define FOOT(b) (*(uint32_t *)((char *)(b) + SZ(b) - 4))

static int bin_of(size_t sz) {
    if (sz < 512) return sz >> 3;
    int l = 31 - __builtin_clz(sz);                       /* sz >= 512: l >= 9 */
    return 64 + l - 9;                                    /* <= 86 for 32-bit sizes */
}

static void unlink_free(struct blk *b) {
    if (b->prev) b->prev->next = b->next;
    else {
        int i = bin_of(SZ(b));
        bins[i] = b->next;
        if (!b->next) binmap[i >> 5] &= ~(1u << (i & 31));
    }
    if (b->next) b->next->prev = b->prev;
}

/* b->size holds the size (PREV_FREE clear: callers guarantee the previous block is used) */
static void put_free(struct blk *b) {
    int i = bin_of(SZ(b));
    b->magic = MAGIC_FREE;
    FOOT(b) = SZ(b);
    NEXT(b)->size |= PREV_FREE;
    b->prev = NULL;
    b->next = bins[i];
    if (b->next) b->next->prev = b;
    bins[i] = b;
    binmap[i >> 5] |= 1u << (i & 31);
}

/* turn [b, b+size) into a free block, merging with free neighbours */
static void release(struct blk *b, size_t size) {
    struct blk *nx = (struct blk *)((char *)b + size);
    if (nx->magic == MAGIC_FREE) { unlink_free(nx); size += SZ(nx); nx->magic = 0; }
    if (b->size & PREV_FREE) {
        struct blk *pv = (struct blk *)((char *)b - *(uint32_t *)((char *)b - 4));
        unlink_free(pv);
        size += SZ(pv);
        b->magic = 0;                                     /* interior headers must not look like live blocks */
        b = pv;
    }
    b->size = size;                                       /* previous block of the merged block is used */
    put_free(b);
}

static int grow(size_t need) {
    size_t sz = (need + CHUNK - 1) & ~(size_t)(CHUNK - 1);
    char *p = sbrk(sz);
    if (p == (char *)-1) {
        sz = (need + 4095) & ~(size_t)4095;               /* settle for the bare minimum */
        p = sbrk(sz);
        if (p == (char *)-1) return -1;
    }
    struct blk *b;
    if (heap_end && p == heap_end) {                      /* contiguous: the old sentinel becomes the new block */
        b = (struct blk *)(p - HDR);
        sz += HDR;
    } else {
        size_t skew = (8 - ((uintptr_t)p & 7)) & 7;
        b = (struct blk *)(p + skew);
        sz = (sz - skew) & ~(size_t)7;
        b->size = 0;                                      /* previous block: none */
    }
    sz -= HDR;                                            /* room for the new sentinel */
    heap_end = (char *)b + sz + HDR;
    struct blk *s = (struct blk *)((char *)b + sz);
    s->size = 0;
    s->magic = MAGIC_USED;
    b->magic = MAGIC_USED;
    release(b, sz);
    return 0;
}

/* merge every cached small block back into the bins */
static void flush_fast(void) {
    for (size_t i = 0; i < sizeof fast / sizeof *fast; i++)
        for (struct blk *b = fast[i], *n; b; b = n) {
            n = b->next;
            b->magic = MAGIC_USED;                            /* release() expects a used block */
            release(b, SZ(b));
        }
    memset(fast, 0, sizeof fast);
    fast_bytes = 0;
}

/* carve `need` bytes off the front of free block b (already unlinked); the tail goes back to its bin */
static void *take(struct blk *b, size_t need) {
    size_t have = SZ(b);
    if (have - need >= MINBLK) {
        struct blk *rest = (struct blk *)((char *)b + need);
        rest->size = have - need;
        put_free(rest);
        b->size = (b->size & PREV_FREE) | need;
    } else NEXT(b)->size &= ~PREV_FREE;
    b->magic = MAGIC_USED;
    return (char *)b + HDR;
}

static void *heap_alloc(size_t n) {
    if (n > 0x7fffffff - 64) { errno = ENOMEM; return NULL; }
    if (n >= BIG) {
        size_t sz = (n + HDR + 4095) & ~(size_t)4095;
        struct blk *b = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (b == MAP_FAILED) return NULL;
        b->size = sz;
        b->magic = MAGIC_MMAP;
        return (char *)b + HDR;
    }
    size_t need = ((n + 7) & ~(size_t)7) + HDR;
    if (need < MINBLK) need = MINBLK;
    if (need <= FAST_MAX && fast[need >> 3]) {
        struct blk *b = fast[need >> 3];
        fast[need >> 3] = b->next;
        fast_bytes -= need;
        b->magic = MAGIC_USED;
        return (char *)b + HDR;
    }
    for (int tries = 0; tries < 3; tries++) {
        for (int i = bin_of(need); i < NBINS; i++) {
            uint32_t m = binmap[i >> 5] >> (i & 31);       /* next non-empty bin at or above i */
            if (!m) { i |= 31; continue; }
            i += __builtin_ctz(m);
            for (struct blk *b = bins[i]; b; b = b->next)
                if (SZ(b) >= need) {                      /* exact bins fit on the first look; others first-fit */
                    unlink_free(b);
                    return take(b, need);
                }
        }
        if (tries == 0 && fast_bytes) { flush_fast(); continue; }   /* merging may be all it takes */
        if (grow(need) < 0) break;
    }
    errno = ENOMEM;
    return NULL;
}

static void heap_free(void *p) {
    if (!p) return;
    struct blk *b = (struct blk *)((char *)p - HDR);
    if (b->magic == MAGIC_MMAP) { b->magic = 0; munmap(b, b->size); return; }
    if (b->magic != MAGIC_USED) abort();                  /* double free / wild pointer */
    size_t sz = SZ(b);
    if (sz <= FAST_MAX) {
        b->magic = MAGIC_FAST;
        b->next = fast[sz >> 3];
        fast[sz >> 3] = b;
        if ((fast_bytes += sz) > FAST_KEEP) flush_fast();
        return;
    }
    release(b, sz);
}

size_t malloc_usable_size(void *p) { return p ? SZ((struct blk *)((char *)p - HDR)) - HDR : 0; }

void *calloc(size_t n, size_t sz) {
    if (sz && n > 0x7fffffff / sz) { errno = ENOMEM; return NULL; }
    void *p = malloc(n * sz);
    return p && ((uint32_t *)p)[-1] != MAGIC_MMAP ? memset(p, 0, n * sz) : p;   /* fresh mappings are zero */
}

static void *heap_realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    if (!n) { free(p); return NULL; }
    if (n > 0x7fffffff - 64) { errno = ENOMEM; return NULL; }
    struct blk *b = (struct blk *)((char *)p - HDR);
    if (b->magic == MAGIC_MMAP) {
        if (n + HDR <= b->size && n >= BIG / 2) return p;  /* ponytail: no mremap, growing copies */
        void *q = heap_alloc(n);
        if (q) { memcpy(q, p, n < b->size - HDR ? n : b->size - HDR); heap_free(p); }
        return q;
    }
    if (b->magic != MAGIC_USED) abort();
    size_t need = ((n + 7) & ~(size_t)7) + HDR, size = SZ(b);
    if (need < MINBLK) need = MINBLK;
    if (need > size) {                                    /* try to grow into a free block that follows */
        struct blk *nx = NEXT(b);
        if (nx->magic == MAGIC_FREE && size + SZ(nx) >= need) {
            unlink_free(nx);
            b->size += SZ(nx);                            /* keeps b's PREV_FREE bit */
            size = SZ(b);
            NEXT(b)->size &= ~PREV_FREE;
        }
    }
    if (need <= size) {
        if (size - need >= 64) {                          /* give a worthwhile tail back */
            struct blk *rest = (struct blk *)((char *)b + need);
            b->size = (b->size & PREV_FREE) | need;
            rest->magic = MAGIC_USED;                     /* looks like a used block until release() frees it */
            rest->size = 0;
            release(rest, size - need);
        }
        return p;
    }
    void *q = heap_alloc(n);
    if (!q) return NULL;
    memcpy(q, p, size - HDR);
    heap_free(p);
    return q;
}

/* pthread.c's recursive lock: only present (and only taken) in programs that create threads */
void __libc_lock(void) __attribute__((weak));
void __libc_unlock(void) __attribute__((weak));
#define LOCK() do { if (__libc_lock) __libc_lock(); } while (0)
#define UNLOCK() do { if (__libc_unlock) __libc_unlock(); } while (0)

void *malloc(size_t n) { LOCK(); void *p = heap_alloc(n); UNLOCK(); return p; }
void *realloc(void *p, size_t n) { LOCK(); p = heap_realloc(p, n); UNLOCK(); return p; }
void free(void *p) { LOCK(); heap_free(p); UNLOCK(); }
