/* malloc.c - first-fit allocator over sbrk. Free blocks form an address-ordered list so
 * free() can coalesce with physical neighbours. ponytail: O(free blocks) per malloc/free,
 * never returns memory to the kernel; add size bins / sbrk(-n) trimming if it shows up in profiles. */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>

#define HDR 8
#define MINBLK 16
#define MAGIC_USED 0xA110C8EDu
#define MAGIC_FREE 0xF4EEB10Cu
#define CHUNK 65536

struct blk {
    uint32_t size;          /* whole block including header */
    uint32_t magic;
    struct blk *next;       /* free blocks only; overlaps user data */
};

static struct blk *freelist;

static void insert_free(struct blk *b) {
    struct blk *prev = NULL, *cur = freelist;
    while (cur && cur < b) prev = cur, cur = cur->next;
    b->magic = MAGIC_FREE;
    if (cur && (char *)b + b->size == (char *)cur) {      /* merge with next */
        b->size += cur->size;
        cur = cur->next;
    }
    b->next = cur;
    if (prev && (char *)prev + prev->size == (char *)b) { /* merge with prev */
        prev->size += b->size;
        prev->next = b->next;
    } else if (prev) prev->next = b;
    else freelist = b;
}

static int grow(size_t need) {
    size_t sz = (need + CHUNK - 1) & ~(size_t)(CHUNK - 1);
    char *p = sbrk(sz);
    if (p == (char *)-1) {
        sz = (need + 4095) & ~(size_t)4095;               /* settle for the bare minimum */
        p = sbrk(sz);
        if (p == (char *)-1) return -1;
    }
    size_t skew = (8 - ((uintptr_t)p & 7)) & 7;
    p += skew;
    sz = (sz - skew) & ~(size_t)7;
    struct blk *b = (struct blk *)p;
    b->size = sz;
    insert_free(b);
    return 0;
}

void *malloc(size_t n) {
    if (n > 0x7fffffff - 64) { errno = ENOMEM; return NULL; }
    size_t need = (n + 7) & ~(size_t)7;
    need += HDR;
    if (need < MINBLK) need = MINBLK;
    for (int tries = 0; tries < 2; tries++) {
        struct blk *prev = NULL;
        for (struct blk *b = freelist; b; prev = b, b = b->next) {
            if (b->size < need) continue;
            struct blk *rest;
            if (b->size - need >= MINBLK) {               /* split, keep the tail free */
                rest = (struct blk *)((char *)b + need);
                rest->size = b->size - need;
                rest->magic = MAGIC_FREE;
                rest->next = b->next;
                b->size = need;
            } else rest = b->next;
            if (prev) prev->next = rest; else freelist = rest;
            b->magic = MAGIC_USED;
            return (char *)b + HDR;
        }
        if (grow(need) < 0) break;
    }
    errno = ENOMEM;
    return NULL;
}

void free(void *p) {
    if (!p) return;
    struct blk *b = (struct blk *)((char *)p - HDR);
    if (b->magic != MAGIC_USED) abort();                  /* double free / wild pointer */
    insert_free(b);
}

void *calloc(size_t n, size_t sz) {
    if (sz && n > 0x7fffffff / sz) { errno = ENOMEM; return NULL; }
    void *p = malloc(n * sz);
    return p ? memset(p, 0, n * sz) : NULL;
}

void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    if (!n) { free(p); return NULL; }
    struct blk *b = (struct blk *)((char *)p - HDR);
    size_t have = b->size - HDR;
    if (n <= have) return p;                              /* ponytail: never shrinks in place */
    void *q = malloc(n);
    if (!q) return NULL;
    memcpy(q, p, have);
    free(p);
    return q;
}
