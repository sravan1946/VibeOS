/* sysmon - CPU graph, memory bars, process table with End Task */
#include <ui.h>
#include <design.h>
#include <icons.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <syscall.h>

#define HIST 120
#define MAXP 64
#define SAMPLE_MS 500

static int cpu_hist[HIST], mem_hist[HIST];      /* percent */
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
        if (nhist == HIST) { memmove(cpu_hist, cpu_hist + 1, (HIST - 1) * sizeof(int)); memmove(mem_hist, mem_hist + 1, (HIST - 1) * sizeof(int)); }
        else nhist++;
        cpu_hist[nhist - 1] = cpu_now;
        mem_hist[nhist - 1] = si.mem_total_kb ? (int)((uint64_t)(si.mem_total_kb - si.mem_free_kb) * 100 / si.mem_total_kb) : 0;
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

/* a history chart: thin text2 line over a faint area, on a 25% grid of line rules */
static void chart(struct ui *ui, int x, int y, int w, int h, const int *v) {
    struct canvas *c = ui->c;
    for (int q = 0; q <= 4; q++) gfx_hline(c, x, y + (h - 1) * q / 4, w, UI_LINE);
    struct canvas cc = *c;
    canvas_clip(&cc, x, y, w, h);
    float step = (float)w / (HIST - 1), px = 0, py = 0;
    for (int i = 0; i < nhist; i++) {
        float fx = x + w - (nhist - 1 - i) * step, fy = y + h - 1 - (h - 2) * v[i] / 100.0f;
        if (i) {
            gfx_fill(&cc, (int)px, (int)fy, (int)fx - (int)px, y + h - (int)fy, UI_OV(16));
            gfx_line_aa(&cc, px, py, fx, fy, 1.5f, UI_TEXT2);
        }
        px = fx; py = fy;
    }
}

/* a card with a title, a value on the right, a chart and a caption */
static void chart_card(struct ui *ui, int x, int y, int w, int h, const char *title, const char *val, const int *v, const char *cap) {
    struct canvas *c = ui->c;
    ui_card(ui, x, y, w, h);
    gfx_text(c, T_BODY_B, x + SP_4, y + SP_4, title, UI_TEXT);
    gfx_text(c, T_BODY, x + w - SP_4 - gfx_text_width(T_BODY, val), y + SP_4, val, UI_TEXT2);
    int cy = y + SP_4 + T_BODY->height + SP_3, ch = h - (cy - y) - SP_3 - T_CAPTION->height - SP_4;
    chart(ui, x + SP_4, cy, w - 2 * SP_4, ch, v);
    ui_text_fit(ui, T_CAPTION, x + SP_4, y + h - SP_4 - T_CAPTION->height, w - 2 * SP_4, cap, UI_TEXT3);
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
        int W = w->w, H = w->h, m = SP_5;
        /* top: CPU and memory charts */
        int topH = 152, cw = (W - 3 * m) / 2;
        char v[48], cap[64];
        snprintf(v, sizeof v, "%d%%", cpu_now);
        chart_card(&ui, m, m, cw, topH, "Processor", v, cpu_hist, "Last 60 seconds");
        int pct = si.mem_total_kb ? (int)((uint64_t)(si.mem_total_kb - si.mem_free_kb) * 100 / si.mem_total_kb) : 0;
        snprintf(v, sizeof v, "%u / %u MiB (%d%%)", (si.mem_total_kb - si.mem_free_kb) / 1024, si.mem_total_kb / 1024, pct);
        snprintf(cap, sizeof cap, "Kernel heap %u / %u MiB", si.heap_used_kb / 1024, si.heap_total_kb / 1024);
        chart_card(&ui, 2 * m + cw, m, W - 3 * m - cw, topH, "Memory", v, mem_hist, cap);

        /* process table */
        int hy = m + topH + SP_6;
        char title[48];
        snprintf(title, sizeof title, "Processes (%d)", nprocs);
        gfx_text(c, T_HEADING, m, hy + (H_CTL - T_HEADING->height) / 2, title, UI_TEXT);
        bool have_sel = false;
        for (int i = 0; i < nprocs; i++) if (rows[i].p.pid == sel_pid) have_sel = true;
        if (!have_sel) sel_pid = -1;
        bool can_kill = sel_pid > 1 && sel_pid != getpid();
        int bw = ui_button_w(UI_NOICON, "End task");
        if (ui_button(&ui, W - m - bw, hy, bw, H_CTL, "End task", UI_SECONDARY | (can_kill ? 0 : UI_DISABLED))) {
            char msg[48];
            snprintf(msg, sizeof msg, kill(sel_pid, SIGKILL) == 0 ? "Ended process %d" : "Couldn't end process %d", sel_pid);
            ui_toast(&ui, msg, NULL);
            sample(); last = now;
        }
        ui_tooltip(&ui, W - m - bw, hy, bw, H_CTL, "End task \xc2\xb7 Delete");
        int ty = hy + H_CTL + SP_3, th = H - ty - m, tw = W - 2 * m;
        ui_card(&ui, m, ty, tw, th);
        int x0 = m + SP_4, cpuR = m + tw - SP_4 - SP_4, memR = cpuR - 80, stX = memR - 80 - 80, pidR = x0 + 40, nameX = x0 + 56;
        /* left edge (or right edge for numbers) per column: pid name state mem cpu */
        const int cx[5] = { pidR, nameX, stX, memR, cpuR }, right[5] = { 1, 0, 0, 1, 1 };
        const char *hn[5] = { "PID", "Name", "State", "Memory", "CPU" };
        int colY = ty;
        gfx_hline(c, m, colY + H_ROW - 1, tw, UI_LINE);
        for (int i = 0; i < 5; i++) {
            int lw = gfx_text_width(T_CAPTION_B, hn[i]), hx = right[i] ? cx[i] - lw : cx[i];
            int ax = right[i] ? hx - ICON_SM - SP_1 : hx + lw + SP_1, hx0 = right[i] ? ax : hx;
            bool hov = ui_hover(&ui, hx0 - SP_2, colY, lw + ICON_SM + SP_1 + SP_3, H_ROW);
            if (hov && ui.pressed) { if (sort_col == i) sort_desc = !sort_desc; else { sort_col = i; sort_desc = i >= 3; } sample(); last = now; }
            gfx_text(c, T_CAPTION_B, hx, colY + (H_ROW - T_CAPTION_B->height) / 2, hn[i], sort_col == i || hov ? UI_TEXT : UI_TEXT2);
            if (sort_col == i) ui_icon(&ui, ax, colY + (H_ROW - ICON_SM) / 2, sort_desc ? ICON_CHEVRON_DOWN : ICON_CHEVRON_UP, ICON_SM, UI_TEXT2);
        }
        int ry0 = colY + H_ROW, rh = H_ROW;
        int listH = ty + th - SP_2 - ry0;
        int vis = listH / rh;
        if (ui_hover(&ui, m, ry0, tw, listH) && ui.scroll) scroll += ui.scroll * 2;
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
        canvas_clip(&cc, m, ry0, tw, listH);
        for (int i = scroll; i < nprocs && i < scroll + vis; i++) {
            int y = ry0 + (i - scroll) * rh;
            struct procinfo *p = &rows[i].p;
            bool hov = ui_hover(&ui, m, y, tw, rh) && y + rh <= ry0 + listH, sel = p->pid == sel_pid;
            if (hov && ui.pressed) sel_pid = p->pid;
            if (sel) gfx_fill(&cc, m, y, tw, rh, UI_SELECTION);
            else if (hov) gfx_fill(&cc, m, y, tw, rh, ui_th.hover_ov);
            char b[32];
            int ty2 = y + (rh - T_BODY->height) / 2;
            const struct font *nf = sel ? T_BODY_B : T_BODY;
            snprintf(b, sizeof b, "%d", p->pid);
            gfx_text(&cc, T_BODY, pidR - gfx_text_width(T_BODY, b), ty2, b, UI_TEXT2);
            ui_text_fit(&ui, nf, nameX, ty2, stX - nameX - SP_4, p->name, UI_TEXT);
            gfx_text(&cc, T_BODY, stX, ty2, STATE[p->state & 3], p->state == PS_ZOMBIE ? UI_BAD : UI_TEXT2);
            if (p->mem_kb >= 1024) snprintf(b, sizeof b, "%u.%u MiB", p->mem_kb / 1024, p->mem_kb % 1024 * 10 / 1024);
            else snprintf(b, sizeof b, "%u KiB", p->mem_kb);
            gfx_text(&cc, T_BODY, memR - gfx_text_width(T_BODY, b), ty2, b, UI_TEXT);
            snprintf(b, sizeof b, "%d%%", rows[i].cpu);
            gfx_text(&cc, T_BODY, cpuR - gfx_text_width(T_BODY, b), ty2, b, p->pid == 0 ? UI_TEXT2 : UI_TEXT);
        }
        ui_vscroll(&ui, m + tw - SP_4, ry0, listH, nprocs, vis, &scroll);
        ui_end(&ui);
    } while (ui_poll(&ui, SAMPLE_MS));
    gui_window_destroy(w);
    return 0;
}
