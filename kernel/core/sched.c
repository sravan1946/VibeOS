/* sched.c - round-robin scheduler.
 * The kernel is non-preemptive: a task in kernel mode runs until it blocks or
 * yields; the timer only forces a switch when returning to user mode. */
#include "proc.h"

struct task *current;
static struct task *tasks, *idle_task;
static int next_pid = 1;
static volatile bool need_resched;
static int quantum;
#define QUANTUM_MS 10

extern void context_switch(uint32_t *save, uint32_t next);
extern void kthread_entry(void);

/* Lazy FPU: a switch doesn't touch the x87 state, it only sets CR0.TS (unless the next task already owns the
 * registers). The first x87 instruction after that raises #NM; fpu_nm saves the old owner's registers and loads
 * the current task's. fnsave+frstor were ~450 of the ~800 cycles of a switch. */
static struct task *fpu_owner;           /* whose registers are live (0: nobody's image is worth saving) */
static bool ts_set;                      /* CR0.TS as last written: a switch that finds it right skips the cr0 write */
static uint8_t clean_fpu[108] __attribute__((aligned(16)));

static void fpu_nm(struct regs *r) {
    (void)r;
    __asm__ volatile("clts");
    ts_set = false;
    if (fpu_owner == current) return;
    if (fpu_owner) __asm__ volatile("fnsave %0" : "=m"(fpu_owner->fpu));
    __asm__ volatile("frstor %0" :: "m"(current->fpu));
    fpu_owner = current;
}

/* make t's x87 state the live one (before the kernel fnsave/frstor/fninit's it on t's behalf) */
void fpu_ensure(struct task *t) {
    uint32_t f = irq_save();
    __asm__ volatile("clts");
    ts_set = false;
    if (fpu_owner != t) {
        if (fpu_owner) __asm__ volatile("fnsave %0" : "=m"(fpu_owner->fpu));
        __asm__ volatile("frstor %0" :: "m"(t->fpu));
        fpu_owner = t;
    }
    irq_restore(f);
}

static void fpu_init(void) {
    uint32_t cr0 = read_cr0();
    cr0 &= ~0x4u;                 /* EM */
    cr0 |= 0x22u;                 /* MP | NE */
    __asm__ volatile("mov %1, %%cr0; fninit; fnsave %0; frstor %0" : "+m"(clean_fpu) : "r"(cr0));
    isr_register(7, fpu_nm);
}

void sched_init(void) {
    fpu_init();
    idle_task = kzalloc(sizeof *idle_task);
    idle_task->pid = 0;
    idle_task->proc = idle_task;
    idle_task->nthreads = 1;
    idle_task->state = PS_RUNNING;
    strlcpy(idle_task->name, "idle", sizeof idle_task->name);
    idle_task->dir = vmm_kernel_dir();
    strcpy(idle_task->cwd, "/");
    tasks = current = idle_task;
    fpu_owner = idle_task;
}

static struct task *task_alloc(const char *name) {
    struct task *t = kzalloc(sizeof *t);
    if (!t) return 0;
    t->kstack = (uint32_t)kmalloc(KSTACK_SIZE);
    if (!t->kstack) { kfree(t); return 0; }
    t->pid = next_pid++;
    t->ppid = current ? current->proc->pid : 0;
    t->proc = t;
    t->nthreads = 1;
    strlcpy(t->name, name, sizeof t->name);
    t->dir = vmm_kernel_dir();
    strcpy(t->cwd, "/");
    memcpy(t->fpu, clean_fpu, sizeof clean_fpu);
    return t;
}

static void task_link(struct task *t) {
    uint32_t f = irq_save();
    t->next = tasks->next;        /* insert after idle */
    tasks->next = t;
    t->state = PS_READY;
    irq_restore(f);
}

/* push the context_switch frame: edi esi ebx ebp eflags ret */
static uint32_t *push_switch_frame(uint32_t *sp, uint32_t ret, uint32_t ebx, uint32_t esi) {
    *--sp = ret;
    *--sp = 0x002;                /* eflags, IF off; the entry path enables it */
    *--sp = 0;                    /* ebp */
    *--sp = ebx;
    *--sp = esi;
    *--sp = 0;                    /* edi */
    return sp;
}

struct task *kthread_create(const char *name, int (*fn)(void *), void *arg) {
    struct task *t = task_alloc(name);
    if (!t) return 0;
    uint32_t *sp = (uint32_t *)(t->kstack + KSTACK_SIZE);
    sp = push_switch_frame(sp, (uint32_t)kthread_entry, (uint32_t)fn, (uint32_t)arg);
    t->kesp = (uint32_t)sp;
    task_link(t);
    return t;
}

/* used by proc.c: a task whose first switch lands in isr_return with frame r */
struct task *task_create_user(const char *name, const struct regs *r) {
    extern void isr_return(void);
    struct task *t = task_alloc(name);
    if (!t) return 0;
    t->user = true;
    uint32_t *sp = (uint32_t *)(t->kstack + KSTACK_SIZE);
    sp = (uint32_t *)((uint8_t *)sp - sizeof *r);
    memcpy(sp, r, sizeof *r);
    sp = push_switch_frame(sp, (uint32_t)isr_return, 0, 0);
    t->kesp = (uint32_t)sp;
    return t;                     /* caller fills in the rest, then task_start() */
}

void task_start(struct task *t) { task_link(t); }

void task_unlink_free(struct task *t) {
    uint32_t f = irq_save();
    for (struct task **pp = &tasks; *pp; pp = &(*pp)->next)
        if (*pp == t) { *pp = t->next; break; }
    if (fpu_owner == t) fpu_owner = 0;
    irq_restore(f);
    kfree((void *)t->kstack);
    kfree(t);
}

struct task *task_find(int pid) {
    for (struct task *t = tasks; t; t = t->next)
        if (t->pid == pid) return t;
    return 0;
}

struct task *task_list(void) { return tasks; }
uint32_t idle_ms(void) { return idle_task->cpu_ms; }

/* libc's errno in t's process, through the identity map (0 if unthreaded or the page is gone) */
static int *uerrno_word(struct task *t) {
    uint32_t va = t->proc->uerrno;
    return va ? (int *)vmm_translate(t->dir, va) : 0;
}

void schedule(void) {
    uint32_t f = irq_save();
    need_resched = false;
    quantum = QUANTUM_MS;
    struct task *prev = current, *next = 0;
    /* round robin starting after current; idle only when nothing else is ready */
    struct task *start = prev->next ? prev->next : tasks, *t = start;
    do {
        if (t != idle_task && t->state == PS_READY) { next = t; break; }
        t = t->next ? t->next : tasks;
    } while (t != start);
    if (!next) next = (prev->state == PS_RUNNING && prev != idle_task) ? prev : idle_task;
    if (prev->state == PS_RUNNING) prev->state = PS_READY;
    next->state = PS_RUNNING;
    if (next != prev) {
        /* threads share libc's errno word: park the outgoing thread's value, install the incoming one's.
         * ponytail: single CPU only; with SMP this needs real TLS (a %gs segment per thread) */
        int *e;
        if (prev->state != PS_ZOMBIE && (e = uerrno_word(prev))) prev->uerrno_val = *e;
        current = next;
        tss_set_kernel_stack(next->kstack + KSTACK_SIZE);
        if (read_cr3() != next->dir) write_cr3(next->dir);
        if ((e = uerrno_word(next))) *e = next->uerrno_val;
        if (next == fpu_owner) { if (ts_set) { __asm__ volatile("clts"); ts_set = false; } }
        else if (!ts_set) { __asm__ volatile("mov %%cr0, %%eax; or $8, %%eax; mov %%eax, %%cr0" ::: "eax"); ts_set = true; }
        context_switch(&prev->kesp, next->kesp);
    }
    irq_restore(f);
}

void yield(void) { schedule(); }

int sleep_on(void *chan, uint32_t timeout_ms) {
    if (sig_deliverable(current)) return -EINTR;
    current->chan = chan;
    current->wake_at = timeout_ms ? ticks_ms + timeout_ms : 0;
    current->state = PS_BLOCKED;
    schedule();
    current->chan = 0;
    current->wake_at = 0;
    return sig_deliverable(current) ? -EINTR : 0;
}

static const struct vnode_ops *keyed_ops[8];
void poll_keyed_ops(const struct vnode_ops *ops) {
    int i = 0;
    while (i < 8 && keyed_ops[i] && keyed_ops[i] != ops) i++;
    if (i < 8) keyed_ops[i] = ops;
}
bool poll_keyed(const struct vnode *v) {
    for (int i = 0; i < 8 && keyed_ops[i]; i++) if (keyed_ops[i] == v->ops) return true;
    return false;
}

extern char poll_chan;
void wakeup(void *chan) {
    uint32_t f = irq_save();
    for (struct task *t = tasks; t; t = t->next) {
        if (t->state != PS_BLOCKED) continue;
        if (t->chan == chan) { t->state = PS_READY; continue; }
        if (t->chan != &poll_chan) continue;
        bool hit = t->poll_any;                   /* a poll() sleeper: only if one of its files can have changed */
        for (int i = 0; !hit && i < t->npoll; i++) hit = t->poll_keys[i] == chan;
        if (hit) t->state = PS_READY;
    }
    irq_restore(f);
}

void sleep_ms(uint32_t ms) {
    uint64_t until = ticks_ms + ms;
    uint32_t f = irq_save();
    while (ticks_ms < until)
        if (sleep_on(&current->wake_at, (uint32_t)(until - ticks_ms)) < 0) break;
    irq_restore(f);
}

/* called from the PIT IRQ every millisecond */
void sched_tick(void) {
    current->cpu_ms++;
    for (struct task *t = tasks; t; t = t->next)
        if (t->state == PS_BLOCKED && t->wake_at && t->wake_at <= ticks_ms) t->state = PS_READY;
    if (--quantum <= 0) need_resched = true;
}

/* last thing before returning from any interrupt/syscall */
void sched_irq_exit(struct regs *r) {
    if ((r->cs & 3) != 3) return;
    if (need_resched) schedule();
    if (sig_deliverable(current)) proc_deliver_signals(r);
}

/* the boot thread becomes the idle task */
NORETURN void sched_idle(void) {
    for (;;) {
        proc_reap_orphans();
        sti();
        ext2_flush_dirty();
        hlt();
        schedule();
    }
}
