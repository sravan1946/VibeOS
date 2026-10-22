/* sysmon - CPU graph, memory bars, process table with End Task */
#include <ui.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <syscall.h>

#define HIST 120
#define MAXP 64
#define SAMPLE_MS 500

static int cpu_hist[HIST];
static int nhist;
struct row { struct procinfo p; int cpu; };       /* cpu = % since the previous sample */
static struct row rows[MAXP];
static int nprocs;
static uint32_t prev_up, prev_idle;
static struct { int pid; uint32_t cpu_ms; } prevp[MAXP];
static int nprev;
static int cpu_now;
static struct sysinfo si;

static int sort_col = 0, sort_desc;          /* 0 pid, 1 name, 2 state, 3 mem, 4 cpu */
static int sel_pid = -1, scroll;
static char status[96];
static uint32_t status_until;

static int cmp(const void *a, const void *b) {
    const struct row *x = a, *y = b;
    int r;
    switch (sort_col) {
    case 1: r = strcmp(x->p.name, y->p.name); break;
    case 2: r = x->p.state - y->p.state; break;
    case 3: r = (int)x->p.mem_kb - (int)y->p.mem_kb; break;
    case 4: r = x->cpu - y->cpu; break;
    default: r = x->p.pid - y->p.pid;
    }
    if (!r) r = x->p.pid - y->p.pid;
    return sort_desc ? -r : r;
}

static void sample(void) {
    sysinfo(&si);
    uint32_t dup = si.uptime_ms - prev_up, didle = si.idle_ms - prev_idle;
    if (prev_up && dup) {
        int pct = 100 - (int)(didle * 100 / dup);
        cpu_now = pct < 0 ? 0 : pct > 100 ? 100 : pct;
        if (nhist == HIST) memmove(cpu_hist, cpu_hist + 1, (HIST - 1) * sizeof(int)); else nhist++;
        cpu_hist[nhist - 1] = cpu_now;
    }
    static struct procinfo pl[MAXP];
    int n = proclist(pl, MAXP);
    if (n < 0) n = 0;
    for (int i = 0; i < n; i++) {                 /* per process CPU = delta cpu_ms / delta wall time */
        rows[i].p = pl[i];
        rows[i].cpu = 0;
        for (int j = 0; j < nprev; j++)
            if (prevp[j].pid == pl[i].pid && dup) {
                int pc = (int)((pl[i].cpu_ms - prevp[j].cpu_ms) * 100 / dup);
                rows[i].cpu = pc < 0 ? 0 : pc > 100 ? 100 : pc;
            }
    }
    nprev = n;
    for (int i = 0; i < n; i++) { prevp[i].pid = pl[i].pid; prevp[i].cpu_ms = pl[i].cpu_ms; }
    qsort(rows, n, sizeof rows[0], cmp);
    nprocs = n;
    prev_up = si.uptime_ms;
    prev_idle = si.idle_ms;
}

static void bar(struct ui *ui, int x, int y, int w, const char *label, uint32_t used_kb, uint32_t total_kb, color_t col) {
    struct canvas *c = ui->c;
    char buf[96];
    int pct = total_kb ? (int)((uint64_t)used_kb * 100 / total_kb) : 0;
    gfx_text(c, &font_ui_bold, x, y, label, UI_TEXT);
    snprintf(buf, sizeof buf, "%u / %u MiB  (%d%%)", used_kb / 1024, total_kb / 1024, pct);
    gfx_text(c, &font_ui, x + w - gfx_text_width(&font_ui, buf), y, buf, UI_TEXT_DIM);
    gfx_round_fill(c, x, y + 24, w, 12, 6, UI_INPUT);
    int fw = w * pct / 100;
    if (fw < 12 && pct > 0) fw = 12;
    if (pct > 0) gfx_round_fill(c, x, y + 24, fw, 12, 6, pct > 90 ? UI_BAD : pct > 75 ? UI_WARN : col);
}

static void graph(struct ui *ui, int x, int y, int w, int h) {
    struct canvas *c = ui->c;
    ui_panel(ui, x, y, w, h);
    gfx_text(c, &font_ui_bold, x + 16, y + 12, "CPU", UI_TEXT);
    char buf[16];
    snprintf(buf, sizeof buf, "%d%%", cpu_now);
    gfx_text(c, &font_big, x + 16, y + 34, buf, cpu_now > 85 ? UI_BAD : UI_ACCENT);
    int gx = x + 16, gy = y + 12, gw = w - 32, gh = h - 24;
    if (gw < 100) return;
    int lx = x + 120;                              /* graph sits to the right of the number */
    gx = lx; gw = x + w - 16 - lx;
    gfx_round_fill(c, gx, gy, gw, gh, 6, UI_INPUT);
    for (int q = 1; q < 4; q++) gfx_fill(c, gx + 1, gy + gh * q / 4, gw - 2, 1, UI_OV(14));
    struct canvas cc = *c;
    canvas_clip(&cc, gx, gy, gw, gh);
    float step = (float)gw / (HIST - 1);
    float px = 0, py = 0;
    for (int i = 0; i < nhist; i++) {
        float fx = gx + gw - (nhist - 1 - i) * step;
        float fy = gy + gh - 2 - (gh - 6) * cpu_hist[i] / 100.0f;
        int ix = (int)fx;
        int top = (int)fy;
        gfx_fill(&cc, ix, top, (int)step + 1, gy + gh - top, UI_ACC(36));
        if (i) gfx_line_aa(&cc, px, py, fx, fy, 2.0f, UI_ACCENT);
        px = fx; py = fy;
    }
    gfx_round_rect(c, gx, gy, gw, gh, 6, UI_BORDER);
    gfx_text(c, &font_ui, x + 16, y + h - 28, "last 60 s", UI_TEXT_DIM);
}

static const char *const STATE[] = { "Running", "Ready", "Sleeping", "Zombie" };

int main(void) {
    struct gui_window *w = gui_window_create("System Monitor", 680, 560, 0);
    if (!w) { perror("sysmon"); return 1; }
    struct ui ui;
    ui_init(&ui, w);
    sample();
    uint32_t last = uptime_ms();
    do {
        uint32_t now = uptime_ms();
        if (now - last >= SAMPLE_MS) { sample(); last = now; }
        ui_clear(&ui);
        struct canvas *c = ui.c;
        int W = w->w, H = w->h, m = 14;
        /* top: graph + memory */
        int topH = 128;
        int gw = (W - 3 * m) * 3 / 5;
        graph(&ui, m, m, gw, topH);
        int mx = m + gw + m, mw = W - mx - m;
        ui_panel(&ui, mx, m, mw, topH);
        bar(&ui, mx + 16, m + 12, mw - 32, "Memory", (si.mem_total_kb - si.mem_free_kb), si.mem_total_kb, UI_ACCENT);
        bar(&ui, mx + 16, m + 12 + 56, mw - 32, "Kernel heap", si.heap_used_kb, si.heap_total_kb, UI_GOOD);

        /* process table */
        int ty = m + topH + m;
        int th = H - ty - m;
        ui_panel(&ui, m, ty, W - 2 * m, th);
        int hdrY = ty + 8, hdrH = 28;
        int btnW = 110;
        char title[48];
        snprintf(title, sizeof title, "Processes (%d)", nprocs);
        gfx_text(c, &font_ui_bold, m + 16, hdrY + 5, title, UI_TEXT);
        bool have_sel = false;
        for (int i = 0; i < nprocs; i++) if (rows[i].p.pid == sel_pid) have_sel = true;
        if (!have_sel) sel_pid = -1;
        int bx = W - m - 16 - btnW;
        bool can_kill = sel_pid > 1 && sel_pid != getpid();
        if (can_kill) {
            if (ui_button(&ui, bx, hdrY - 2, btnW, 30, "End Task", UI_ACCENT_BTN)) {
                int r = kill(sel_pid, SIGKILL);
                snprintf(status, sizeof status, r == 0 ? "Ended process %d" : "Could not end process %d", sel_pid);
                status_until = now + 3000;
                sample(); last = now;
            }
        } else {
            gfx_round_fill(c, bx, hdrY - 2, btnW, 30, UI_RADIUS, UI_OV(10));
            gfx_text_center(c, &font_ui, bx, hdrY + 4, btnW, "End Task", UI_TEXT_DIM);
        }
        if (status[0] && now < status_until) {
            int sw = gfx_text_width(&font_ui, status);
            gfx_text(c, &font_ui, bx - sw - 14, hdrY + 5, status, UI_WARN);
        }
        int colY = hdrY + hdrH + 4;
        int tx = m + 12, tw = W - 2 * m - 24;
        int cols[5] = { tx + 6, tx + 66, tx + tw - 250, tx + tw - 160, tx + tw - 60 };   /* left edges: pid name state mem cpu */
        const char *hn[5] = { "PID", "Name", "State", "Memory", "CPU" };
        gfx_fill(c, tx, colY + 24, tw, 1, UI_BORDER);
        for (int i = 0; i < 5; i++) {
            int cw = gfx_text_width(&font_ui_bold, hn[i]) + 16;
            bool hov = ui_hover(&ui, cols[i] - 6, colY, cw + 6, 24);
            if (hov && ui.pressed) { if (sort_col == i) sort_desc = !sort_desc; else { sort_col = i; sort_desc = i >= 3; } sample(); last = now; }
            gfx_text(c, &font_ui_bold, cols[i], colY + 3, hn[i], sort_col == i || hov ? UI_TEXT : UI_TEXT_DIM);
            if (sort_col == i) {                                        /* sort chevron */
                int ax = cols[i] + gfx_text_width(&font_ui_bold, hn[i]) + 8, ay = colY + 12;
                int d = sort_desc ? 1 : -1;
                gfx_line_aa(c, ax - 4, ay - 2 * d, ax, ay + 2 * d, 1.8f, UI_ACCENT);
                gfx_line_aa(c, ax, ay + 2 * d, ax + 4, ay - 2 * d, 1.8f, UI_ACCENT);
            }
        }
        int ry0 = colY + 28, rh = 26;
        int listH = ty + th - 8 - ry0;
        int vis = listH / rh;
        if (ui_hover(&ui, m, ry0, W - 2 * m, listH) && ui.scroll) scroll += ui.scroll * 2;
        if (scroll > nprocs - vis) scroll = nprocs - vis;
        if (scroll < 0) scroll = 0;
        struct gui_event k;
        while (ui_key(&ui, &k)) {
            int si_ = -1;
            for (int i = 0; i < nprocs; i++) if (rows[i].p.pid == sel_pid) si_ = i;
            if (k.key == KEY_DOWN && si_ < nprocs - 1) sel_pid = rows[si_ + 1].p.pid;
            else if (k.key == KEY_UP && si_ > 0) sel_pid = rows[si_ - 1].p.pid;
            else if (k.key == KEY_DELETE && can_kill) { kill(sel_pid, SIGKILL); sample(); last = now; }
        }
        struct canvas cc = *c;
        canvas_clip(&cc, m, ry0, W - 2 * m, listH);
        for (int i = scroll; i < nprocs && i < scroll + vis + 1; i++) {
            int y = ry0 + (i - scroll) * rh;
            struct procinfo *p = &rows[i].p;
            bool hov = ui_hover(&ui, tx, y, tw - 12, rh) && y + rh <= ry0 + listH;
            if (hov && ui.pressed) sel_pid = p->pid;
            if (p->pid == sel_pid) gfx_round_fill(&cc, tx, y, tw - 12, rh - 2, 6, UI_ACC(80));
            else if (hov) gfx_round_fill(&cc, tx, y, tw - 12, rh - 2, 6, UI_OV(14));
            else if (i & 1) gfx_round_fill(&cc, tx, y, tw - 12, rh - 2, 6, UI_OV(5));
            char b[32];
            int ty2 = y + (rh - font_ui.height) / 2 - 1;
            snprintf(b, sizeof b, "%d", p->pid);
            gfx_text(&cc, &font_mono, cols[0], ty2 + 1, b, UI_TEXT_DIM);
            ui_text_fit(&ui, &font_ui, cols[1], ty2, cols[2] - cols[1] - 10, p->name, UI_TEXT);
            color_t sc = p->state == PS_RUNNING ? UI_GOOD : p->state == PS_ZOMBIE ? UI_BAD : p->state == PS_READY ? UI_ACCENT : UI_TEXT_DIM;
            gfx_circle_fill(&cc, cols[2] + 4, y + rh / 2 - 1, 3, sc);
            gfx_text(&cc, &font_ui, cols[2] + 14, ty2, STATE[p->state & 3], UI_TEXT);
            if (p->mem_kb >= 1024) snprintf(b, sizeof b, "%u.%u MiB", p->mem_kb / 1024, p->mem_kb % 1024 * 10 / 1024);
            else snprintf(b, sizeof b, "%u KiB", p->mem_kb);
            gfx_text(&cc, &font_ui, cols[3], ty2, b, UI_TEXT);
            snprintf(b, sizeof b, "%d%%", rows[i].cpu);
            gfx_text(&cc, &font_ui, cols[4], ty2, b, p->pid == 0 ? UI_TEXT_DIM : rows[i].cpu > 50 ? UI_WARN : UI_TEXT);
        }
        ui_vscroll(&ui, W - m - 14, ry0, listH, nprocs, vis, &scroll);
        ui_end(&ui);
    } while (ui_poll(&ui, SAMPLE_MS));
    gui_window_destroy(w);
    return 0;
}
