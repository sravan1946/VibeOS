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

static void fpu_init(void) {
    uint32_t cr0 = read_cr0();
    cr0 &= ~0x4u;                 /* EM */
    cr0 |= 0x22u;                 /* MP | NE */
    __asm__ volatile("mov %0, %%cr0; fninit" :: "r"(cr0));
}

void sched_init(void) {
    fpu_init();
    idle_task = kzalloc(sizeof *idle_task);
    idle_task->pid = 0;
    idle_task->state = PS_RUNNING;
    strlcpy(idle_task->name, "idle", sizeof idle_task->name);
    idle_task->dir = vmm_kernel_dir();
    strcpy(idle_task->cwd, "/");
    __asm__ volatile("fnsave %0; frstor %0" : "+m"(idle_task->fpu));
    tasks = current = idle_task;
}

static struct task *task_alloc(const char *name) {
    struct task *t = kzalloc(sizeof *t);
    if (!t) return 0;
    t->kstack = (uint32_t)kmalloc(KSTACK_SIZE);
    if (!t->kstack) { kfree(t); return 0; }
    t->pid = next_pid++;
    t->ppid = current ? current->pid : 0;
    strlcpy(t->name, name, sizeof t->name);
    t->dir = vmm_kernel_dir();
    strcpy(t->cwd, "/");
    /* a clean FPU image: fninit then save it */
    uint32_t f = irq_save();
    uint8_t tmp[108] __attribute__((aligned(16)));
    __asm__ volatile("fnsave %0" : "=m"(tmp));
    __asm__ volatile("fninit; fnsave %0" : "=m"(t->fpu));
    __asm__ volatile("frstor %0" :: "m"(tmp));
    irq_restore(f);
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
        current = next;
        tss_set_kernel_stack(next->kstack + KSTACK_SIZE);
        if (read_cr3() != next->dir) write_cr3(next->dir);
        __asm__ volatile("fnsave %0" : "=m"(prev->fpu));
        __asm__ volatile("frstor %0" :: "m"(next->fpu));
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

extern char poll_chan;
void wakeup(void *chan) {
    uint32_t f = irq_save();
    /* ponytail: poll() sleepers wake on every event and re-check; fine at this task count */
    for (struct task *t = tasks; t; t = t->next)
        if (t->state == PS_BLOCKED && (t->chan == chan || t->chan == &poll_chan)) t->state = PS_READY;
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
        hlt();
        schedule();
    }
}
