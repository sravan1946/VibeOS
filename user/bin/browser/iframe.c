/* iframe.c - <iframe>: drawing a frame's document inside its box, giving it layout and its own scroll position, routing the mouse
 * wheel, clicks and keys to the frame under the pointer or the one that holds focus, and the window.parent/top/postMessage side of the
 * JS API (js_prelude_fr). The browsing contexts themselves (a JSContext and node tree each) are js.c's; a frame here is a struct frame
 * holding the document that js.c's tree was laid out into. A frame is laid out at the size of its <iframe> box when it is first drawn
 * and again when its own scripts change it or that size does: the parent never lays out for it.
 * ponytail: no text selection or find inside a frame, <select> dropdowns inside a frame don't open, a form POST from a frame is a GET,
 * wheel scrolling snaps instead of easing, links ignore target= (they navigate the frame they are in). */
#include "browser.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <design.h>

/* form.c: swap its field-state table with a frame's */
void fs_swap(struct fstate **pf, int *pn);

/* the frame's world: its JS state selected and its form table in place. Nested: leaving restores the parent frame (or the page). */
static int enter(struct frame *f) {
    if (f->in) return 0;
    js_use(f);
    fs_swap(&f->fs, &f->nfs);
    f->in = 1;
    return 1;
}
static void leave(struct frame *f, int own) {
    if (!own) return;
    fs_swap(&f->fs, &f->nfs);
    f->in = 0;
    js_use(f->par);
}

void frame_relayout(struct frame *f) {
    if (f->w <= 0 || f->h <= 0) return;
    struct frame *was = js_cur();
    int own = enter(f);
    js_use(f);                                            /* (the caller may be another frame's script) */
    int ci = -1, fnode = 0;
    for (int i = 0; i < f->doc.nct; i++) if (f->doc.ct[i].ord == f->fo) { ci = i; fnode = f->doc.ct[i].node; }
    js_sync();
    fs_reset();
    doc_layout_js(&f->doc, f->w, js_frame_url(f));
    js_attach(&f->doc);
    f->fo = -1;
    for (int i = 0; fnode && i < f->doc.nct; i++) if (f->doc.ct[i].node == fnode) f->fo = f->doc.ct[i].ord;
    (void)ci;
    f->lw = f->w; f->lh = f->h; f->redo = 0; f->ready = 1;
    int mx = f->doc.height - 24 > f->h ? f->doc.height - 24 - f->h : 0;   /* (layout adds 24px of slack at the bottom) */
    if (f->sy > mx) f->sy = f->ty = mx;
    leave(f, own);
    js_use(was);
}

static void sel_draw(struct canvas *c, struct frame *f, int ox, int oy);
static void submit_in(struct frame *f, int form, int btn);
void frames_dirty(void) { for (int i = 0, n = js_nframes(); i < n; i++) js_frame_nth(i)->redo = 1; }

void frame_paint(struct canvas *c, const struct doc *d, const struct item *it, int x, int y) {
    struct frame *f = js_frame_find(d, it->node);
    int w = it->w, h = it->h;
    if (!f) { gfx_fill(c, x, y, w, h, RGB(0xFF, 0xFF, 0xFF)); return; }
    f->bx = it->x; f->by = it->y;
    if (w != f->w || h != f->h) { f->w = w; f->h = h; f->redo = 1; }
    if ((f->redo || !f->ready) && w > 0 && h > 0) frame_relayout(f);
    if (!f->ready) { gfx_fill(c, x, y, w, h, RGB(0xFF, 0xFF, 0xFF)); return; }
    int own = enter(f);
    doc_draw_c(c, &f->doc, x, y, w, h, f->sy, f->hover, 0, f->focus ? f->fo : -1, f->caret);
    if (f->selp) sel_draw(c, f, x, y);
    leave(f, own);
    int ph = f->doc.height - 24;
    if (ph > h) {                                         /* a slim scroll thumb */
        int th = h * h / ph, ty = (int)((long long)f->sy * (h - th) / (ph - h));
        struct canvas cc = *c;
        canvas_clip(&cc, x, y, w, h);
        gfx_fill(&cc, x + w - 5, y + ty, 4, th < 12 ? 12 : th, RGBA(0, 0, 0, 90));
    }
}

/* ---------- pumping ---------- */
void frames_pump(void) {
    for (int k = 0; k < js_nframes(); k++) {
        struct frame *f = js_frame_nth(k);
        if (!f) break;
        int own = enter(f);
        js_viewport(f->sy, f->w > 0 ? f->w : 300, f->h > 0 ? f->h : 150);
        int d = js_pump();
        char nav[URL_MAX];
        const char *n = js_nav();
        nav[0] = 0;
        if (n) strlcpy(nav, n, sizeof nav);
        js_scroll_req(); js_anchor_req(); js_url_change();
        int sf = js_submit_req();                           /* form.submit() in the frame */
        for (int i = 0; sf && i < f->doc.nfm; i++) if (f->doc.fm[i].node == sf) { submit_in(f, i, -1); break; }
        int fr = js_focus_req();
        if (fr) for (int i = 0; i < f->doc.nct; i++) if (f->doc.ct[i].node == fr) { f->fo = f->doc.ct[i].ord; f->caret = f->doc.ct[i].kind == C_TEXT ? 0 : 0; }
        char *al = js_alert();
        free(al);
        leave(f, own);
        if (d) f->redo = 1;
        if (nav[0] && !js_frame_is_worker(f)) js_frame_navigate(f, nav);
        if (f->redo && f->w > 0 && f->ready) frame_relayout(f);
    }
}

/* ---------- an open <select>: its list over the frame ---------- */
#define SELRH 20
static int sel_geom(struct frame *f, int *px, int *py, int *pw, int *rows) {      /* frame viewport coordinates; 0 = no list */
    if (!f->selp || f->selp - 1 >= f->doc.nct) return 0;
    const struct ctl *c = &f->doc.ct[f->selp - 1];
    if (c->item < 0) return 0;
    const struct item *it = &f->doc.it[c->item];
    *rows = c->no < 8 ? c->no : 8;
    *px = it->x; *pw = it->w < 100 ? 100 : it->w;
    *py = it->y - f->sy + it->h;
    if (*py + *rows * SELRH > f->h) *py = it->y - f->sy - *rows * SELRH;
    return *rows > 0;
}
static void sel_draw(struct canvas *c, struct frame *f, int ox, int oy) {
    int px, py, pw, rows;
    if (!sel_geom(f, &px, &py, &pw, &rows)) return;
    const struct ctl *t = &f->doc.ct[f->selp - 1];
    struct fstate *fs = fs_get(&f->doc, f->selp - 1);
    struct canvas cc = *c;
    canvas_clip(&cc, ox, oy, f->w, f->h);
    gfx_fill(&cc, ox + px, oy + py, pw, rows * SELRH, RGB(0xFF, 0xFF, 0xFF));
    gfx_rect(&cc, ox + px, oy + py, pw, rows * SELRH, RGB(0x76, 0x76, 0x76));
    for (int r = 0; r < rows; r++) {
        int oi = f->sel_sc + r;
        if (oi >= t->no) break;
        if (oi == fs->sel) gfx_fill(&cc, ox + px + 1, oy + py + r * SELRH, pw - 2, SELRH, RGB(0xC8, 0xDC, 0xFF));
        gfx_text(&cc, T_BODY, ox + px + 6, oy + py + r * SELRH + (SELRH - T_BODY->height) / 2, f->doc.txt + f->doc.op[t->o0 + oi].lab, RGB(0x1A, 0x1A, 0x22));
    }
}

/* ---------- input ---------- */
static struct frame *kfocus;                                /* the frame that has the keyboard (NULL = the page) */
static struct frame *hovered;                               /* the frame the pointer was last over */

static int ctl_by_ord(struct frame *f, int ord) { for (int i = 0; i < f->doc.nct; i++) if (f->doc.ct[i].ord == ord) return i; return -1; }

static void submit_in(struct frame *f, int form, int btn) {
    char url[URL_MAX], body[16384];
    if (form < 0 || js_event(f->doc.fm[form].node, "submit", 0, 0, 0)) return;
    int post = form_submit(&f->doc, form, btn, js_frame_url(f), url, sizeof url, body, sizeof body);
    char u[URL_MAX], tg[64];
    strlcpy(u, url, sizeof u);
    const char *t = js_form_target(f->doc.fm[form].node);
    strlcpy(tg, t ? t : "", sizeof tg);
    if (!js_goto_target(f, tg, u, post ? body : 0)) js_frame_navigate_post(f, u, post ? body : 0);
}

static void blur_in(struct frame *f) {                      /* focus leaves the frame's field */
    if (f->fo < 0) return;
    int ci = ctl_by_ord(f, f->fo);
    f->fo = -1;
    if (ci >= 0) { js_event(f->doc.ct[ci].node, "blur", 0, 0, 0); js_event(f->doc.ct[ci].node, "focusout", 0, 0, 0); }
}

/* a pointer event at (dx, dy) in the parent's document, over frame f. ev: 0 move, 1 press, 2 release. */
static void point(struct frame *f, int dx, int dy, int ev) {
    int lx = dx - f->bx, ly = dy - f->by;
    if (lx < 0 || ly < 0 || lx >= f->w || ly >= f->h || !f->ready) return;
    int own = enter(f);
    int x = lx, y = doc_unstick(&f->doc, ly + f->sy, f->sy), node = doc_node_at(&f->doc, x, y);
    struct frame *ch = node ? js_frame_find(&f->doc, node) : 0;
    if (ev == 1 && !ch) node = js_hit(x, y, node);
    if (ch) {                                                /* a frame inside the frame */
        f->hover = -1;
        point(ch, x, y, ev);
        leave(f, own);
        return;
    }
    js_viewport(f->sy, f->w, f->h);
    int ci = doc_ctl_at(&f->doc, x, y), hv = doc_hit(&f->doc, x, y);
    f->hover = hv;
    if (ev == 0) {
        if (node != f->pnode) { if (f->pnode) js_event(f->pnode, "mouseout", 0, x, y); if (node) js_event(node, "mouseover", 0, x, y); f->pnode = node; }
        if (x != f->pmx || y != f->pmy) { f->pmx = x; f->pmy = y; js_event(node, "mousemove", 0, x, y); }
    } else if (ev == 2) js_event(node, "mouseup", 0, x, y);
    else {
        kfocus = f; f->focus = 1;
        if (f->selp) {                                       /* a click while a list is open: pick a row, or just close it */
            int sx, sy, sw, rows;
            if (sel_geom(f, &sx, &sy, &sw, &rows) && lx >= sx && lx < sx + sw && ly >= sy && ly < sy + rows * SELRH) {
                int oi = f->sel_sc + (ly - sy) / SELRH, sci = f->selp - 1;
                fs_get(&f->doc, sci)->sel = oi;
                js_event(f->doc.ct[sci].node, "input", 0, 0, 0); js_event(f->doc.ct[sci].node, "change", 0, 0, 0);
            }
            f->selp = 0;
            leave(f, own);
            return;
        }
        blur_in(f);
        js_event(node, "mousedown", 0, x, y);
        if (ci >= 0) {
            const struct ctl *c = &f->doc.ct[ci];
            const struct item *it = c->item >= 0 ? &f->doc.it[c->item] : 0;
            if ((c->kind == C_TEXT || c->kind == C_PASS || c->kind == C_AREA) && it) {
                f->fo = c->ord; f->caret = ctl_caret_at(&f->doc, ci, x - it->x, y - it->y); fs_get(&f->doc, ci)->anc1 = f->caret + 1;
                js_event(node, "click", 0, x, y);
                js_event(node, "focus", 0, 0, 0); js_event(node, "focusin", 0, 0, 0);
            } else if (c->kind == C_SELECT) {
                if (!js_event(node, "click", 0, x, y)) { f->selp = ci + 1; f->sel_sc = fs_get(&f->doc, ci)->sel - 3 < 0 ? 0 : fs_get(&f->doc, ci)->sel - 3; }
            } else if (c->kind == C_BTN) {
                if (!js_event(node, "click", 0, x, y)) { if (c->sub == 1) submit_in(f, c->form, ci); else if (c->sub == 2) form_reset(&f->doc, c->form); }
            } else if (c->kind == C_CHECK || c->kind == C_RADIO) {
                struct fstate *fs = fs_get(&f->doc, ci);
                int was = fs->on;
                ctl_toggle(&f->doc, ci);
                if (js_event(node, "click", 0, x, y)) fs->on = was;
                else if (fs->on != was) { js_event(node, "input", 0, x, y); js_event(node, "change", 0, x, y); }
            }
        } else if (hv >= 0) {
            if (!js_event(node, "click", 0, x, y)) {
                char u[URL_MAX], tg[64];
                url_resolve(js_frame_url(f), f->doc.link[hv], u, sizeof u);
                const char *t = js_link_target(node);
                strlcpy(tg, t ? t : "", sizeof tg);
                leave(f, own);
                if (!js_goto_target(f, tg, u, 0)) js_frame_navigate(f, u);
                return;
            }
        } else js_event(node, "click", 0, x, y);
    }
    leave(f, own);
}

static struct frame *frame_under(const struct doc *d, int node) { return node ? js_frame_find(d, node) : 0; }

/* the page's pointer handler found <iframe> `node` under (dx, dy) (document coordinates of d); true if a frame took the event */
int frame_pointer(const struct doc *d, int node, int dx, int dy, int ev) {
    struct frame *f = frame_under(d, node);
    if (!f) return 0;
    if (ev == 0 && hovered && hovered != f) frame_leave();
    if (ev == 0) hovered = f;
    point(f, dx, dy, ev);
    return 1;
}
void frame_leave(void) {                                    /* the pointer left the frames (or moved to another) */
    struct frame *f = hovered;
    hovered = 0;
    if (!f) return;
    int own = enter(f);
    f->hover = -1;
    if (f->pnode) { js_event(f->pnode, "mouseout", 0, 0, 0); f->pnode = 0; }
    f->pmx = f->pmy = -1;
    leave(f, own);
}

/* the wheel over a frame: scroll it if it has room in that direction (the frame inside first), else leave it to the page */
static int wheel_in(struct frame *f, int dx, int dy, int delta) {
    int lx = dx - f->bx, ly = dy - f->by;
    if (lx < 0 || ly < 0 || lx >= f->w || ly >= f->h || !f->ready) return 0;
    int own = enter(f);
    int node = doc_node_at(&f->doc, lx, ly + f->sy), r = 0;
    struct frame *ch = node ? js_frame_find(&f->doc, node) : 0;
    if (ch) r = wheel_in(ch, lx, ly + f->sy, delta);
    if (!r) {
        int mx = f->doc.height - 24 > f->h ? f->doc.height - 24 - f->h : 0, ny = f->sy + delta * 64;
        ny = ny < 0 ? 0 : ny > mx ? mx : ny;
        if (ny != f->sy) { f->sy = f->ty = ny; r = 1; }
    }
    leave(f, own);
    return r;
}
int frame_wheel(const struct doc *d, int node, int dx, int dy, int delta) {
    struct frame *f = frame_under(d, node);
    return f ? wheel_in(f, dx, dy, delta) : 0;
}

struct frame *frame_focused(void) { return kfocus; }
void frame_gone(struct frame *f) {
    for (struct frame *p = kfocus; p; p = p->par) if (p == f) { kfocus = 0; break; }
    for (struct frame *p = hovered; p; p = p->par) if (p == f) { hovered = 0; break; }
}
void frame_unfocus(void) {                                  /* a click on the page itself */
    struct frame *f = kfocus;
    kfocus = 0;
    if (!f) return;
    for (struct frame *p = f; p; p = p->par) {
        int own = enter(p);
        p->focus = 0;
        blur_in(p);
        leave(p, own);
    }
}

/* a key press for the focused frame */
void frame_key(const struct gui_event *k) {
    struct frame *f = kfocus;
    if (!f) return;
    int own = enter(f);
    int ci = f->fo >= 0 ? ctl_by_ord(f, f->fo) : -1, node = ci >= 0 ? f->doc.ct[ci].node : 0;
    int shift = k->mods & MOD_SHIFT;
    if (f->selp) {                                           /* keys for the open list */
        int sci = f->selp - 1;
        struct fstate *fs = fs_get(&f->doc, sci);
        if (k->key == KEY_DOWN && fs->sel < f->doc.ct[sci].no - 1) fs->sel++;
        else if (k->key == KEY_UP && fs->sel > 0) fs->sel--;
        else if (k->key == KEY_ENTER || k->key == ' ') { js_event(f->doc.ct[sci].node, "input", 0, 0, 0); js_event(f->doc.ct[sci].node, "change", 0, 0, 0); f->selp = 0; }
        else if (k->key == KEY_ESC) f->selp = 0;
        if (f->selp) { if (fs->sel < f->sel_sc) f->sel_sc = fs->sel; if (fs->sel >= f->sel_sc + 8) f->sel_sc = fs->sel - 7; }
        leave(f, own);
        return;
    }
    if (js_event(node, "keydown", k, 0, 0)) { leave(f, own); return; }
    if (ci >= 0) {
        struct fstate *fs = fs_get(&f->doc, ci);
        char *before = fs->val ? strdup(fs->val) : 0;
        int enter_key = ctl_key(&f->doc, ci, k, &f->caret);
        if (before && fs->val && strcmp(before, fs->val)) js_event(node, "input", 0, 0, 0);
        free(before);
        js_event(node, "keyup", k, 0, 0);
        if (enter_key && !js_event(node, "keypress", k, 0, 0)) submit_in(f, f->doc.ct[ci].form, -1);
    } else {
        int page = f->h - 40;
        switch (k->key) {
        case KEY_DOWN: f->sy += 48; break;
        case KEY_UP: f->sy -= 48; break;
        case KEY_PGDN: f->sy += page; break;
        case KEY_PGUP: f->sy -= page; break;
        case ' ': f->sy += shift ? -page : page; break;
        case KEY_HOME: f->sy = 0; break;
        case KEY_END: f->sy = f->doc.height; break;
        }
        int mx = f->doc.height - 24 > f->h ? f->doc.height - 24 - f->h : 0;
        f->sy = f->sy < 0 ? 0 : f->sy > mx ? mx : f->sy;
        f->ty = f->sy;
    }
    leave(f, own);
}

/* ---------- the JS side ---------- */
const char js_prelude_fr[] = R"JS(
(function (g) {
'use strict';
const d = g.__d, V = g.__v, D = V.classes;
const self = d.fself();
// ---- structured clone: tagged JSON, rebuilt in the receiving realm ----
const hex = u8 => { let s = ''; for (let i = 0; i < u8.length; i++) s += (u8[i] < 16 ? '0' : '') + u8[i].toString(16); return s; };
const unhex = s => { const u = new Uint8Array(s.length >> 1); for (let i = 0; i < u.length; i++) u[i] = parseInt(s.substr(i * 2, 2), 16); return u; };
let MP = null;                                  // MessagePort (made once the page's own preludes are done: see V.late)
const PM = new Map(), mkport = id => PM.get(id) || new MP(id);
function ser(v, tr) {
    const seen = new Map(), xfer = new Set(Array.isArray(tr) ? tr : []);
    const bad = x => new g.DOMException(String(typeof x === 'function' ? x : typeof x) + ' could not be cloned.', 'DataCloneError');
    const enc = (x, depth) => {
        if (x === null || typeof x === 'boolean' || typeof x === 'string') return x;
        if (typeof x === 'number') return Number.isFinite(x) && !Object.is(x, -0) ? x : { $: 'n', v: Object.is(x, -0) ? '-0' : String(x) };
        if (x === undefined) return { $: 'u' };
        if (typeof x === 'bigint') return { $: 'bi', v: String(x) };
        if (typeof x === 'function' || typeof x === 'symbol' || depth > 100) throw bad(x);
        if (seen.has(x)) return { $: 'r', v: seen.get(x) };
        seen.set(x, seen.size);
        if (x instanceof Date) return { $: 'd', v: +x };
        if (x instanceof RegExp) return { $: 'x', v: x.source, f: x.flags };
        if (x instanceof ArrayBuffer) return { $: 'ab', v: hex(new Uint8Array(x)) };
        if (ArrayBuffer.isView(x)) return { $: 'ta', t: x.constructor.name, v: hex(new Uint8Array(x.buffer, x.byteOffset, x.byteLength)) };
        if (x instanceof Map) return { $: 'm', v: [...x].map(([a, b]) => [enc(a, depth + 1), enc(b, depth + 1)]) };
        if (x instanceof Set) return { $: 's', v: [...x].map(a => enc(a, depth + 1)) };
        if (x instanceof Error) return { $: 'e', n: x.name, v: x.message };
        if (MP && x instanceof MP) { if (!xfer.has(x)) throw bad(x); return { $: 'p', v: x.__pid }; }
        if (x.nodeType !== undefined && typeof x.nodeName === 'string' || x === x.window) throw bad(x);
        if (Array.isArray(x)) return { $: 'a', v: Array.from(x, a => enc(a, depth + 1)) };
        const o = {};
        for (const k of Object.keys(x)) o[k] = enc(x[k], depth + 1);
        return { $: 'o', v: o };
    };
    return JSON.stringify({ v: enc(v, 0), t: [...xfer].filter(p => p && p.__pid).map(p => p.__pid) });
}
function deser(json) {
    const objs = [];
    const dec = x => {
        if (x === null || typeof x !== 'object') return x;
        switch (x.$) {
        case 'n': return x.v === '-0' ? -0 : Number(x.v);
        case 'u': return undefined;
        case 'p': return mkport(x.v);
        case 'bi': return BigInt(x.v);
        case 'r': return objs[x.v];
        case 'd': { const o = new Date(x.v); objs.push(o); return o; }
        case 'x': { const o = new RegExp(x.v, x.f); objs.push(o); return o; }
        case 'ab': { const o = unhex(x.v).buffer; objs.push(o); return o; }
        case 'ta': { const b = unhex(x.v).buffer, C = x.t === 'DataView' ? DataView : g[x.t]; const o = new C(b); objs.push(o); return o; }
        case 'e': { const o = new (g[x.n] || Error)(x.v); objs.push(o); return o; }
        case 'm': { const o = new Map(); objs.push(o); for (const [a, b] of x.v) o.set(dec(a), dec(b)); return o; }
        case 's': { const o = new Set(); objs.push(o); for (const a of x.v) o.add(dec(a)); return o; }
        case 'a': { const o = []; objs.push(o); for (const a of x.v) o.push(dec(a)); return o; }
        default: { const o = {}; objs.push(o); for (const k of Object.keys(x.v)) o[k] = dec(x.v[k]); return o; }
        }
    };
    const e = JSON.parse(json);
    return { data: dec(e.v), ports: (e.t || []).map(mkport) };
}
// ---- a window we may not look inside ----
const XW = new Map();
function xwin(fid) {
    let p = XW.get(fid);
    if (p) return p;
    const err = k => new g.DOMException('Blocked a frame with origin "' + d.forigin(self) + '" from accessing a cross-origin frame. (' + String(k) + ')', 'SecurityError');
    const loc = new Proxy({}, {
        get(t, k) { if (k === 'replace') return u => d.fnav(fid, String(u)); throw err('location.' + String(k)); },
        set(t, k, v) { if (k === 'href') { d.fnav(fid, String(v)); return true; } throw err('location.' + String(k)); },
        has() { throw err('location'); }, ownKeys() { throw err('location'); },
    });
    const get = {
        window: () => p, self: () => p, frames: () => p, closed: () => false, length: () => 0, opener: () => null,
        top: () => d.fwindow(0), parent: () => { const q = d.fparent(fid); return q < 0 ? p : d.fwindow(q); }, location: () => loc,
        postMessage: () => (m, o, tr) => d.fpost(fid, m, o, tr), focus: () => () => {}, blur: () => () => {}, close: () => () => {},
    };
    p = new Proxy({}, {
        get(t, k) { if (typeof k === 'string' && get[k]) return get[k](); if (k === Symbol.toStringTag) return 'Window'; if (k === Symbol.toPrimitive) return undefined; throw err('property "' + String(k) + '"'); },
        set(t, k, v) { if (k === 'location') { d.fnav(fid, String(v)); return true; } throw err('property "' + String(k) + '"'); },
        has(t, k) { return typeof k === 'string' && !!get[k]; },
        getPrototypeOf() { return null; }, ownKeys() { throw err('ownKeys'); }, getOwnPropertyDescriptor() { throw err('getOwnPropertyDescriptor'); },
        defineProperty() { throw err('defineProperty'); }, deleteProperty() { throw err('deleteProperty'); },
    });
    XW.set(fid, p);
    return p;
}
const isW = d.isworker(), WK = new Map();
function msg(json, origin, from, port) {
    let r;
    try { r = deser(json); } catch (e) { g.dispatchEvent(new D.MessageEvent('messageerror', { origin })); return; }
    if (port > 0) { const p = PM.get(port); if (p) p.__deliver(r.data, r.ports); return; }       // a message on a MessagePort
    const ev = new D.MessageEvent('message', { data: r.data, origin, source: from < 0 || isW ? null : d.fwindow(from) });
    ev.ports = r.ports;
    if (from < 0) { const w = WK.get(-from); if (w) w.dispatchEvent(ev); return; }   // from a worker we made
    g.dispatchEvent(ev);
}
d.reg(id => V.wrap(id), msg, xwin, ser, (name, fid) => { Object.defineProperty(g, name, { get() { return d.fwindow(fid) || undefined; }, configurable: true }); });
// ---- workers: a Worker here; in a worker, no window or document, importScripts and a postMessage to the creator ----
if (isW) {
    for (const k of ['window', 'document', 'parent', 'top', 'frames', 'frameElement', 'opener', 'alert', 'confirm', 'prompt', 'open', 'print']) try { delete g[k]; } catch (e) {}
    g.importScripts = (...us) => { for (const u of us) { const a = d.resolve(String(u)), s = d.syncGet(a); if (s === null || s === undefined) throw new g.DOMException('Failed to execute \'importScripts\' on \'WorkerGlobalScope\': The script at \'' + a + '\' failed to load.', 'NetworkError'); (0, g.eval)(s + '\n//# sourceURL=' + a); } };
    g.postMessage = (m, tr) => d.wsend(ser(m, tr), tr);
    g.close = () => {};
    g.WorkerGlobalScope = class WorkerGlobalScope {};
    Object.setPrototypeOf(g, g.WorkerGlobalScope.prototype);
} else {
    class Worker extends D.EventTarget {
        constructor(url) {
            super();
            if (arguments.length < 1) throw new TypeError("Failed to construct 'Worker': 1 argument required, but only 0 present.");
            this.__fid = d.wnew(String(url));
            if (this.__fid < 0) throw new g.DOMException("Failed to construct 'Worker'", 'SecurityError');
            WK.set(this.__fid, this);
        }
        postMessage(m, tr) { d.fpost(this.__fid, m, '*', tr); }
        terminate() { d.wterm(this.__fid); WK.delete(this.__fid); }
    }
    for (const t of ['message', 'error', 'messageerror']) Object.defineProperty(Worker.prototype, 'on' + t, { get() { return this['__on' + t] || null; }, set(f) { const k = '__on' + t; if (this[k]) this.removeEventListener(t, this[k]); this[k] = typeof f === 'function' ? f : null; if (this[k]) this.addEventListener(t, f); }, configurable: true, enumerable: true });
    Object.defineProperty(g, 'Worker', { value: Worker, writable: true, configurable: true });
}
// ---- window relationships ----
const def = (k, get) => Object.defineProperty(g, k, { get, configurable: true, enumerable: true });
if (!isW) {
def('parent', () => self ? d.fwindow(d.fparent()) : g);
def('top', () => self ? d.fwindow(0) : g);
def('frames', () => g);
def('length', () => d.fcount());
def('frameElement', () => d.fel());
for (let i = 0; i < 16; i++) def(i, () => { const f = d.fchild(i); return f < 0 ? undefined : d.fwindow(f); });
Object.defineProperty(g, 'postMessage', { value: d.postMessage, writable: true, configurable: true });
}
// ---- <iframe> ----
const P = D.HTMLIFrameElement.prototype;
const attr = (n, a) => Object.defineProperty(P, n, { get() { return this.getAttribute(a || n) || ''; }, set(v) { this.setAttribute(a || n, String(v)); }, configurable: true, enumerable: true });
for (const n of ['srcdoc', 'align', 'scrolling', 'frameBorder']) attr(n, n.toLowerCase());
Object.defineProperties(P, {
    contentWindow: { get() { return d.fwin(this.__id); }, configurable: true, enumerable: true },
    contentDocument: { get() { const w = d.fwin(this.__id); if (!w) return null; try { return w.document; } catch (e) { return null; } }, configurable: true, enumerable: true },
});
P.getSVGDocument = function () { return null; };
// MessageChannel / MessagePort across contexts (after web_js.c, which has a same-realm stand-in)
V.late = () => {
    class MessagePort extends D.EventTarget {
        constructor(id) {
            super();
            Object.defineProperties(this, { __pid: { value: id }, __q: { value: [] }, __st: { value: false, writable: true } });
            PM.set(id, this);
        }
        postMessage(m, tr) { const l = Array.isArray(tr) ? tr : tr && tr.transfer; d.ppost(this.__pid, ser(m, l), l); }
        start() { if (this.__st) return; this.__st = true; for (const q of this.__q.splice(0)) this.__fire(q); }
        close() { PM.delete(this.__pid); }
        __deliver(data, ports) { if (this.__st) this.__fire([data, ports]); else this.__q.push([data, ports]); }
        __fire(q) { const ev = new D.MessageEvent('message', { data: q[0] }); ev.ports = q[1]; this.dispatchEvent(ev); }
    }
    for (const t of ['message', 'messageerror']) Object.defineProperty(MessagePort.prototype, 'on' + t, { get() { return this['__on' + t] || null; }, set(f) { const k = '__on' + t; if (this[k]) this.removeEventListener(t, this[k]); this[k] = typeof f === 'function' ? f : null; if (this[k]) { this.addEventListener(t, f); if (t === 'message') this.start(); } }, configurable: true, enumerable: true });
    class MessageChannel { constructor() { const [a, b] = d.pnew(); this.port1 = new MessagePort(a); this.port2 = new MessagePort(b); } }
    MP = MessagePort;
    Object.defineProperty(g, 'MessagePort', { value: MessagePort, writable: true, configurable: true });
    Object.defineProperty(g, 'MessageChannel', { value: MessageChannel, writable: true, configurable: true });
};
})(globalThis);
)JS";
