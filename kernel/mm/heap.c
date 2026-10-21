/* heap.c - kmalloc: first-fit over one fixed region with neighbour coalescing.
 * ponytail: O(blocks) search with a rover; swap for size classes if it shows up in profiles. */
#include "kernel.h"

struct blk { uint32_t size; uint32_t free; struct blk *next, *prev; };   /* size incl. header */
#define HDR sizeof(struct blk)
static struct blk *first, *rover;
static size_t hsize, hused;

void heap_init(uint32_t start, uint32_t size) {
    first = rover = (struct blk *)start;
    first->size = size; first->free = 1; first->next = first->prev = 0;
    hsize = size; hused = 0;
}

void *kmalloc(size_t n) {
    if (!n) n = 1;
    uint32_t need = ALIGN_UP(n + HDR, 16);
    uint32_t f = irq_save();
    struct blk *b = rover;
    for (int pass = 0; pass < 2; pass++) {
        for (; b; b = b->next) {
            if (!b->free || b->size < need) continue;
            if (b->size >= need + HDR + 32) {          /* split */
                struct blk *r = (struct blk *)((char *)b + need);
                r->size = b->size - need; r->free = 1;
                r->next = b->next; r->prev = b;
                if (b->next) b->next->prev = r;
                b->next = r; b->size = need;
            }
            b->free = 0;
            hused += b->size;
            rover = b->next ? b->next : first;
            irq_restore(f);
            return b + 1;
        }
        b = first;
    }
    irq_restore(f);
    kprintf("kmalloc: out of memory (%u bytes)\n", (unsigned)n);
    return 0;
}

void *kzalloc(size_t n) { void *p = kmalloc(n); if (p) memset(p, 0, n); return p; }

void kfree(void *p) {
    if (!p) return;
    uint32_t f = irq_save();
    struct blk *b = (struct blk *)p - 1;
    assert(!b->free);
    b->free = 1;
    hused -= b->size;
    if (b->next && b->next->free) {
        struct blk *n = b->next;
        if (rover == n) rover = b;
        b->size += n->size; b->next = n->next;
        if (n->next) n->next->prev = b;
    }
    if (b->prev && b->prev->free) {
        struct blk *pv = b->prev;
        if (rover == b) rover = pv;
        pv->size += b->size; pv->next = b->next;
        if (b->next) b->next->prev = pv;
    }
    irq_restore(f);
}

void *krealloc(void *p, size_t n) {
    if (!p) return kmalloc(n);
    struct blk *b = (struct blk *)p - 1;
    if (b->size - HDR >= n) return p;
    void *q = kmalloc(n);
    if (q) { memcpy(q, p, b->size - HDR); kfree(p); }
    return q;
}

size_t heap_used(void) { return hused; }
size_t heap_size(void) { return hsize; }
