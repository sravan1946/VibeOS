/* mmap.c - demand paging for user memory: brk, the main stack and mmap'd ranges map a page on its
 * first touch. mmap'd ranges are a sorted list of vmas per process; brk and the stack are implicit. */
#include "proc.h"

struct vma { uint32_t start, end; int prot, flags; struct vnode *vn; uint32_t off; struct vma *next; };

/* PROT_NONE pages stay present but supervisor-only, so mprotect back keeps their contents */
static uint32_t pte_flags(int prot) { return prot ? PTE_U | (prot & PROT_WRITE ? PTE_W : 0) : 0; }

/* ponytail: linear list, no merging of neighbours; a tree when processes map thousands of ranges */
static struct vma *vma_find(struct task *p, uint32_t va) {
    for (struct vma *v = p->vmas; v && v->start <= va; v = v->next)
        if (va < v->end) return v;
    return 0;
}

int uvm_fault(uint32_t va, bool write, uint32_t esp) {
    struct task *p = current->proc;
    if (!current->user || va < USER_BASE || va >= USER_TOP) return -1;
    uint32_t a = ALIGN_DOWN(va, PAGE_SIZE), flags = PTE_U | PTE_W;
    if (vmm_translate(p->dir, a)) return -1;                      /* present: a protection fault */
    struct vma *v = 0;
    if (a >= p->brk_start && a < ALIGN_UP(p->brk, PAGE_SIZE)) {}
    else if (a >= USER_STACK_TOP - STACK_MAX && a < USER_STACK_TOP) { if (esp && va + 65536 < esp) return -1; }
    else if (!(v = vma_find(p, a)) || !v->prot || (write && !(v->prot & PROT_WRITE))) return -1;
    else flags = pte_flags(v->prot);
    uint32_t f = pmm_alloc();                                     /* zeroed */
    if (!f) { kprintf("[mm] %s[%d]: out of memory\n", current->name, current->pid); return -1; }
    uint32_t off = v && v->vn ? v->off + (a - v->start) : 0;
    if (v && v->vn && off < v->vn->size) {                        /* the file read may sleep: re-check after */
        struct vnode *vn = v->vn;
        vnode_get(vn);
        uint32_t ie = irq_save();
        sti();
        int r = vn->ops->read(vn, off, (void *)f, MIN(PAGE_SIZE, vn->size - off));
        irq_restore(ie);
        vnode_put(vn);
        if (r < 0 || vma_find(p, a) != v || vmm_translate(p->dir, a)) { pmm_free(f); return r < 0 ? -1 : 0; }
    }
    if (vmm_map(p->dir, a, f, flags) < 0) { pmm_free(f); return -1; }
    return 0;
}

static void insert(struct task *p, struct vma *n) {
    struct vma **pp = &p->vmas;
    while (*pp && (*pp)->start < n->start) pp = &(*pp)->next;
    n->next = *pp;
    *pp = n;
}

/* put a vma boundary at a */
static int split(struct task *p, uint32_t a) {
    struct vma *v = vma_find(p, a);
    if (!v || v->start == a) return 0;
    struct vma *n = kmalloc(sizeof *n);
    if (!n) return -ENOMEM;
    *n = *v;
    n->start = a;
    if (n->vn) { n->off += a - v->start; vnode_get(n->vn); }
    v->end = a;
    v->next = n;
    return 0;
}

static void drop(struct vma *v) {
    if (v->vn) vnode_put(v->vn);
    kfree(v);
}

/* the highest free gap of len bytes between the break and MMAP_TOP */
static uint32_t place(struct task *p, uint32_t len) {
    uint32_t prev = ALIGN_UP(p->brk, PAGE_SIZE), best = 0;
    for (struct vma *v = p->vmas; ; v = v->next) {
        uint32_t top = v ? v->start : MMAP_TOP;
        if (top > prev && top - prev >= len) best = top - len;
        if (!v) return best;
        prev = MAX(prev, v->end);
    }
}

uint32_t uvm_brk_limit(void) { return current->proc->vmas ? current->proc->vmas->start : MMAP_TOP; }

static bool bad_range(uint32_t addr, uint32_t len) {
    return (addr & 0xFFF) || !len || len > USER_TOP - USER_BASE || addr < USER_BASE || addr > USER_TOP - ALIGN_UP(len, PAGE_SIZE);
}

int uvm_munmap(uint32_t addr, uint32_t len) {
    struct task *p = current->proc;
    if (bad_range(addr, len)) return -EINVAL;
    uint32_t e = addr + ALIGN_UP(len, PAGE_SIZE);
    if (split(p, addr) < 0 || split(p, e) < 0) return -ENOMEM;
    /* ponytail: another thread's syscall already past its user check may still write into the freed frames */
    for (struct vma **pp = &p->vmas; *pp; ) {
        struct vma *v = *pp;
        if (v->start < addr || v->end > e) { pp = &v->next; continue; }
        vmm_unmap_range(p->dir, v->start, v->end - v->start, true);
        *pp = v->next;
        drop(v);
    }
    return 0;
}

int uvm_mmap(uint32_t addr, uint32_t len, int prot, int flags, int fd, uint32_t off) {
    struct task *p = current->proc;
    int type = flags & (MAP_SHARED | MAP_PRIVATE);
    if (!len || len > MMAP_TOP - USER_BASE || (off & 0xFFF) || (prot & ~7) || (type != MAP_SHARED && type != MAP_PRIVATE) ||
        (flags & ~(MAP_SHARED | MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS)))
        return -EINVAL;
    len = ALIGN_UP(len, PAGE_SIZE);
    struct vnode *vn = 0;
    if (!(flags & MAP_ANONYMOUS)) {
        struct file *f = fd_get(fd);
        if (!f) return -EBADF;
        if (!S_ISREG(f->vn->mode) || !f->vn->ops->read) return -ENODEV;
        if ((f->flags & O_ACCMODE) == O_WRONLY) return -EACCES;
        if (type == MAP_SHARED && (prot & PROT_WRITE)) return -ENODEV;   /* ponytail: no write-back to files */
        vn = f->vn;
    }
    /* ponytail: MAP_SHARED anonymous memory is shared by threads only; no fork, and no fd to pass it on */
    if (flags & MAP_FIXED) {
        if (addr < ALIGN_UP(p->brk, PAGE_SIZE) || bad_range(addr, len) || addr + len > MMAP_TOP) return -EINVAL;
        int r = uvm_munmap(addr, len);
        if (r < 0) return r;
    } else if (!(addr = place(p, len))) return -ENOMEM;
    struct vma *v = kmalloc(sizeof *v);
    if (!v) return -ENOMEM;
    *v = (struct vma){ addr, addr + len, prot, flags, vn, off, 0 };
    if (vn) vnode_get(vn);
    insert(p, v);
    return addr;
}

/* mmap'd pages change for good; brk, stack and image pages are faulted in first so the change sticks
 * (ponytail: until sbrk shrinks past them) */
int uvm_mprotect(uint32_t addr, uint32_t len, int prot) {
    struct task *p = current->proc;
    if (bad_range(addr, len) || (prot & ~7)) return -EINVAL;
    uint32_t e = addr + ALIGN_UP(len, PAGE_SIZE);
    for (uint32_t a = addr; a < e; ) {                            /* all of it must be mapped */
        struct vma *v = vma_find(p, a);
        if (v && v->vn && (v->flags & MAP_SHARED) && (prot & PROT_WRITE)) return -EACCES;
        if (v) { a = v->end; continue; }
        if (!vmm_translate(p->dir, a) && uvm_fault(a, false, 0) < 0) return -ENOMEM;
        a += PAGE_SIZE;
    }
    if (split(p, addr) < 0 || split(p, e) < 0) return -ENOMEM;
    for (struct vma *v = p->vmas; v && v->start < e; v = v->next)
        if (v->start >= addr) v->prot = prot;
    for (uint32_t a = addr; a < e; a += PAGE_SIZE) vmm_protect(p->dir, a, pte_flags(prot));
    return 0;
}

void uvm_free(struct task *p) {
    while (p->vmas) {
        struct vma *v = p->vmas;
        p->vmas = v->next;
        drop(v);
    }
}
