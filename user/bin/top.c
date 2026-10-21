/* top : live process list, 1 s refresh.  up/down select, k kill (SIGTERM) / K (SIGKILL) with y/n confirm,
 *       s cycle sort (cpu, mem, pid), q quit.  CPU % = cpu_ms delta / wall delta between samples. */
#include <tui.h>
#include <syscall.h>
#include <signal.h>
#include <time.h>

#define MAXP 128

struct row { struct procinfo p; int pct10; };       /* pct10: CPU % in tenths */

static struct procinfo cur[MAXP], prev[MAXP];
static int ncur, nprev, sel, sort_by;
static uint32_t prev_ms, prev_idle;
static int cpu10;                                    /* whole-machine busy, tenths of a percent */
static struct row rows_[MAXP];
static int nrows;

static int cmp(const void *a, const void *b) {
    const struct row *x = a, *y = b;
    if (sort_by == 1) return x->p.mem_kb < y->p.mem_kb ? 1 : x->p.mem_kb > y->p.mem_kb ? -1 : x->p.pid - y->p.pid;
    if (sort_by == 2) return x->p.pid - y->p.pid;
    return y->pct10 != x->pct10 ? y->pct10 - x->pct10 : y->p.cpu_ms != x->p.cpu_ms ? (y->p.cpu_ms > x->p.cpu_ms ? 1 : -1) : x->p.pid - y->p.pid;
}

static void sample(void) {
    struct sysinfo si;
    sysinfo(&si);
    memcpy(prev, cur, sizeof cur);
    nprev = ncur;
    ncur = proclist(cur, MAXP);
    if (ncur < 0) ncur = 0;
    uint32_t dt = si.uptime_ms - prev_ms;
    if (dt && prev_ms) cpu10 = 1000 - (int)((si.idle_ms - prev_idle) * 1000 / dt);
    if (cpu10 < 0) cpu10 = 0;
    if (cpu10 > 1000) cpu10 = 1000;
    nrows = 0;
    for (int i = 0; i < ncur; i++) {
        if (cur[i].pid == 0) continue;                /* the idle task is the CPU bar's job */
        struct row *r = &rows_[nrows++];
        r->p = cur[i];
        r->pct10 = 0;
        for (int j = 0; j < nprev; j++)
            if (prev[j].pid == cur[i].pid && dt && prev_ms) {
                r->pct10 = (int)((cur[i].cpu_ms - prev[j].cpu_ms) * 1000 / dt);
                break;
            }
    }
    prev_ms = si.uptime_ms;
    prev_idle = si.idle_ms;
    qsort(rows_, nrows, sizeof *rows_, cmp);
}

static void bar(struct sb *o, int pct, int w) {      /* truecolour gradient block bar */
    int f = pct * w / 100;
    for (int i = 0; i < w; i++) {
        if (i < f) {
            int t = i * 255 / (w > 1 ? w - 1 : 1);
            sb_printf(o, "\x1b[38;2;%d;%d;%dm\xe2\x96\x88", t < 128 ? 60 + t * 190 / 128 : 250, t < 128 ? 220 : 220 - (t - 128) * 160 / 127, 90);
        } else sb_puts(o, "\x1b[38;2;60;64;82m\xe2\x96\x91");
    }
    sb_puts(o, "\x1b[0m");
}

static void draw(struct sb *o, int rows, int cols, const char *msg) {
    struct sysinfo si;
    sysinfo(&si);
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    uint32_t s = si.uptime_ms / 1000, used = si.mem_total_kb - si.mem_free_kb;
    int bw = cols > 70 ? 30 : 14;
    sb_puts(o, "\x1b[?25l\x1b[H\x1b[0m");
    sb_printf(o, "\x1b[1mtop\x1b[0m - %02d:%02d:%02d up %u:%02u:%02u, %u tasks\x1b[K\r\n", tm.tm_hour, tm.tm_min, tm.tm_sec,
              s / 3600, s % 3600 / 60, s % 60, si.nprocs);
    sb_printf(o, "CPU  [");
    bar(o, cpu10 / 10, bw);
    sb_printf(o, "] %3d.%d%%\x1b[K\r\n", cpu10 / 10, cpu10 % 10);
    sb_printf(o, "Mem  [");
    bar(o, si.mem_total_kb ? used * 100 / si.mem_total_kb : 0, bw);
    sb_printf(o, "] %u/%u MiB\x1b[K\r\n", used / 1024, si.mem_total_kb / 1024);
    sb_printf(o, "Heap [");
    bar(o, si.heap_total_kb ? si.heap_used_kb * 100 / si.heap_total_kb : 0, bw);
    sb_printf(o, "] %u/%u MiB\x1b[K\r\n\x1b[K\r\n", si.heap_used_kb / 1024, si.heap_total_kb / 1024);
    static const char *const st[] = { "run", "ready", "sleep", "zombie" };
    sb_printf(o, "\x1b[7m%5s %5s %-6s %8s %6s %8s  %-*s\x1b[0m\r\n", "PID", "PPID", "STATE", "MEM(KB)", "CPU%", "TIME", cols > 50 ? cols - 45 : 4, "NAME");
    int avail = rows - 7;
    if (sel >= nrows) sel = nrows - 1;
    if (sel < 0) sel = 0;
    int first = sel >= avail ? sel - avail + 1 : 0;
    for (int r = 0; r < avail; r++) {
        int i = first + r;
        if (i < nrows) {
            const struct procinfo *p = &rows_[i].p;
            uint32_t t = p->cpu_ms / 1000;
            sb_printf(o, "%s%5d %5d %-6s %8u %3d.%d %5u:%02u  %.*s\x1b[K\x1b[0m\r\n", i == sel ? "\x1b[1;48;2;60;50;110m" : "", p->pid, p->ppid,
                      p->state >= 0 && p->state <= 3 ? st[p->state] : "?", p->mem_kb, rows_[i].pct10 / 10, rows_[i].pct10 % 10,
                      t / 60, t % 60, cols > 50 ? cols - 46 : 4, p->name);
        } else sb_puts(o, "\x1b[K\r\n");
    }
    sb_printf(o, "\x1b[%d;1H\x1b[7m%-*.*s\x1b[0m", rows, cols, cols,
              msg ? msg : " q quit  up/down select  k kill  K kill -9  s sort");
    sb_flush(o);
}

int main(void) {
    if (!isatty(1)) { fprintf(stderr, "top: stdout is not a terminal\n"); return 1; }
    int kfd = open("/dev/tty", O_RDWR);
    if (kfd < 0) kfd = 0;
    tui_enter(kfd);
    atexit(tui_leave);
    struct sb o = { 0 };
    char msg[100] = "";
    sample();
    uint32_t next = uptime_ms() + 1000;
    for (;;) {
        int rows, cols;
        tui_size(1, &rows, &cols);
        draw(&o, rows, cols, msg[0] ? msg : NULL);
        msg[0] = 0;
        int wait = (int)(next - uptime_ms());
        int k = tui_key(kfd, wait > 0 ? wait : 0);
        if (uptime_ms() >= next) { sample(); next = uptime_ms() + 1000; }
        switch (k) {
        case 'q': case 'Q': case CTRL('c'): case TK_EOF: return 0;
        case KEY_DOWN: case 'j': sel++; break;
        case KEY_UP: sel--; break;
        case 's': sort_by = (sort_by + 1) % 3; sample(); break;
        case 'k': case 'K': {
            if (sel >= nrows) break;
            int pid = rows_[sel].p.pid, sig = k == 'K' ? SIGKILL : SIGTERM;
            char q[100];
            snprintf(q, sizeof q, " Send %s to PID %d (%s)? y/n", sig == SIGKILL ? "SIGKILL" : "SIGTERM", pid, rows_[sel].p.name);
            draw(&o, rows, cols, q);
            if (tui_key(kfd, -1) == 'y') {
                if (kill(pid, sig) < 0) snprintf(msg, sizeof msg, " kill %d: %s", pid, strerror(errno));
                else snprintf(msg, sizeof msg, " sent signal %d to %d", sig, pid);
            }
            break;
        }
        }
    }
}
