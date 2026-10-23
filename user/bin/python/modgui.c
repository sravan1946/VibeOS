/* modgui.c - the `gui` module: windows over libgui (same drawing set as Lua's gui module, plus events as objects)
 *   w = gui.Window("title", 400, 300)       colours: 0xRRGGBB, 0xAARRGGBB (> 0xFFFFFF) or (r, g, b[, a])
 *   w.fill(c)  w.fill(x, y, w, h, c)  w.rect(x, y, w, h, c, fill=True)  w.line(x0, y0, x1, y1, c, width=1)
 *   w.circle(cx, cy, r, c)  w.pixel(x, y[, c])  w.text(x, y, s, c, font="mono")  w.blit(x, y, buf, w, h, alpha=False)
 *   w.present()  w.poll(ms=0) -> event or None  w.events(ms=0) -> [event, ...]  w.fileno()  w.close()
 *   gui.theme() -> {"window": 0xRRGGBB, "text": ..., "dark": True, ...}: the desktop's Graphite tokens; a "theme"
 *   event arrives when they change (repaint then) */
#include <string.h>
#include <stddef.h>
#include <gui.h>
#include <design.h>

#include "py/mperrno.h"
#include "py/objtuple.h"
#include "vibe_io.h"

typedef struct { mp_obj_base_t base; struct gui_window *w; } win_t;
extern const mp_obj_type_t gui_type_window;

static struct gui_window *W(mp_obj_t o) {
    struct gui_window *w = ((win_t *)MP_OBJ_TO_PTR(o))->w;
    if (!w) mp_raise_ValueError(MP_ERROR_TEXT("window is closed"));
    return w;
}

static color_t col(mp_obj_t o) {
    if (mp_obj_is_int(o)) {
        uint32_t c = mp_obj_get_int_truncated(o);
        return c > 0xFFFFFF ? c : 0xFF000000u | c;
    }
    size_t n;
    mp_obj_t *e;
    mp_obj_get_array(o, &n, &e);
    if (n < 3 || n > 4) mp_raise_ValueError(MP_ERROR_TEXT("colour is an int or (r, g, b[, a])"));
    return RGBA(mp_obj_get_int(e[0]) & 255, mp_obj_get_int(e[1]) & 255, mp_obj_get_int(e[2]) & 255, n == 4 ? mp_obj_get_int(e[3]) & 255 : 255);
}
#define I mp_obj_get_int

static const struct font *font_of(mp_obj_t o) {
    static const char *const names[] = { "mono", "mono_bold", "ui", "ui_bold", "title", "big" };
    static const struct font *const fonts[] = { &font_mono, &font_mono_bold, T_BODY, T_BODY_B, T_TITLE, T_DISPLAY };
    if (o == mp_const_none) return &font_mono;
    const char *s = mp_obj_str_get_str(o);
    for (int i = 0; i < 6; i++) if (!strcmp(s, names[i])) return fonts[i];
    mp_raise_ValueError(MP_ERROR_TEXT("font is mono, mono_bold, ui, ui_bold, title or big"));
}

/* ---- window ---- */
static mp_obj_t win_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    enum { A_title, A_w, A_h, A_resizable };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_title, MP_ARG_REQUIRED | MP_ARG_OBJ }, { MP_QSTR_w, MP_ARG_INT, { .u_int = 400 } },
        { MP_QSTR_h, MP_ARG_INT, { .u_int = 300 } }, { MP_QSTR_resizable, MP_ARG_BOOL, { .u_bool = true } },
    };
    mp_arg_val_t a[4];
    mp_arg_parse_all_kw_array(n_args, n_kw, args, 4, allowed, a);
    win_t *s = mp_obj_malloc_with_finaliser(win_t, type);
    s->w = gui_window_create(mp_obj_str_get_str(a[A_title].u_obj), a[A_w].u_int, a[A_h].u_int, a[A_resizable].u_bool ? 0 : WIN_FLAG_NORESIZE);
    if (!s->w) mp_raise_OSError(errno);
    return MP_OBJ_FROM_PTR(s);
}

static void win_attr(mp_obj_t self, qstr attr, mp_obj_t *dest) {
    if (dest[0] != MP_OBJ_NULL) return;                  /* loads only */
    struct gui_window *w = ((win_t *)MP_OBJ_TO_PTR(self))->w;
    if (attr == MP_QSTR_width) dest[0] = MP_OBJ_NEW_SMALL_INT(w ? w->w : 0);
    else if (attr == MP_QSTR_height) dest[0] = MP_OBJ_NEW_SMALL_INT(w ? w->h : 0);
    else if (attr == MP_QSTR_closed) dest[0] = mp_obj_new_bool(!w);
    else dest[1] = MP_OBJ_SENTINEL;                      /* not ours: look in locals_dict */
}

static mp_obj_t win_close(mp_obj_t self) {
    win_t *s = MP_OBJ_TO_PTR(self);
    if (s->w) { gui_window_destroy(s->w); s->w = NULL; }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(win_close_obj, win_close);
static mp_obj_t win_exit(size_t n, const mp_obj_t *a) { return win_close(a[0]); }
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(win_exit_obj, 4, 4, win_exit);

static mp_obj_t win_fileno(mp_obj_t self) { return MP_OBJ_NEW_SMALL_INT(W(self)->fd); }
static MP_DEFINE_CONST_FUN_OBJ_1(win_fileno_obj, win_fileno);

static mp_obj_t win_set_title(mp_obj_t self, mp_obj_t t) {
    if (gui_set_title(W(self), mp_obj_str_get_str(t)) < 0) mp_raise_OSError(errno);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(win_set_title_obj, win_set_title);

static mp_obj_t win_resize(mp_obj_t self, mp_obj_t w, mp_obj_t h) {   /* the resize event confirms the new size */
    if (gui_resize(W(self), I(w), I(h)) < 0) mp_raise_OSError(errno);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(win_resize_obj, win_resize);

/* ---- drawing ---- */
static mp_obj_t win_fill(size_t n, const mp_obj_t *a) {              /* fill(c) | fill(x, y, w, h, c) */
    struct gui_window *w = W(a[0]);
    if (n == 2) gfx_fill(&w->cv, 0, 0, w->w, w->h, col(a[1]));
    else if (n == 6) gfx_fill(&w->cv, I(a[1]), I(a[2]), I(a[3]), I(a[4]), col(a[5]));
    else mp_raise_TypeError(MP_ERROR_TEXT("fill(colour) or fill(x, y, w, h, colour)"));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(win_fill_obj, 2, 6, win_fill);

static mp_obj_t win_rect(size_t n, const mp_obj_t *a, mp_map_t *kw) {   /* rect(x, y, w, h, c, fill=True) */
    mp_map_elem_t *f = mp_map_lookup(kw, MP_OBJ_NEW_QSTR(MP_QSTR_fill), MP_MAP_LOOKUP);
    if (n != 6) mp_raise_TypeError(MP_ERROR_TEXT("rect(x, y, w, h, colour, fill=True)"));
    bool fill = !f || mp_obj_is_true(f->value);
    (fill ? gfx_fill : gfx_rect)(&W(a[0])->cv, I(a[1]), I(a[2]), I(a[3]), I(a[4]), col(a[5]));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(win_rect_obj, 6, win_rect);

static mp_obj_t win_line(size_t n, const mp_obj_t *a, mp_map_t *kw) {   /* line(x0, y0, x1, y1, c, width=1): wider is anti-aliased */
    mp_map_elem_t *f = mp_map_lookup(kw, MP_OBJ_NEW_QSTR(MP_QSTR_width), MP_MAP_LOOKUP);
    if (n != 6) mp_raise_TypeError(MP_ERROR_TEXT("line(x0, y0, x1, y1, colour, width=1)"));
    mp_float_t wd = f ? mp_obj_get_float(f->value) : 1;
    if (wd == 1) gfx_line(&W(a[0])->cv, I(a[1]), I(a[2]), I(a[3]), I(a[4]), col(a[5]));
    else gfx_line_aa(&W(a[0])->cv, I(a[1]), I(a[2]), I(a[3]), I(a[4]), wd, col(a[5]));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(win_line_obj, 6, win_line);

static mp_obj_t win_circle(size_t n, const mp_obj_t *a) {            /* filled */
    gfx_circle_fill(&W(a[0])->cv, I(a[1]), I(a[2]), I(a[3]), col(a[4]));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(win_circle_obj, 5, 5, win_circle);

static mp_obj_t win_pixel(size_t n, const mp_obj_t *a) {             /* pixel(x, y) -> 0xRRGGBB (0 off-window); pixel(x, y, c) */
    struct gui_window *w = W(a[0]);
    int x = I(a[1]), y = I(a[2]);
    if (n == 4) { gfx_pixel(&w->cv, x, y, col(a[3])); return mp_const_none; }
    return x >= 0 && y >= 0 && x < w->w && y < w->h ? MP_OBJ_NEW_SMALL_INT(w->pixels[y * w->stride + x] & 0xFFFFFF) : MP_OBJ_NEW_SMALL_INT(0);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(win_pixel_obj, 3, 4, win_pixel);

static mp_obj_t win_text(size_t n, const mp_obj_t *a, mp_map_t *kw) {   /* text(x, y, s, c=white, font="mono") -> advance */
    mp_map_elem_t *f = mp_map_lookup(kw, MP_OBJ_NEW_QSTR(MP_QSTR_font), MP_MAP_LOOKUP);
    if (n < 4 || n > 6) mp_raise_TypeError(MP_ERROR_TEXT("text(x, y, s, colour=white, font=\"mono\")"));
    color_t c = n > 4 ? col(a[4]) : RGB(255, 255, 255);
    const struct font *fn = font_of(n > 5 ? a[5] : f ? f->value : mp_const_none);
    return MP_OBJ_NEW_SMALL_INT(gfx_text(&W(a[0])->cv, fn, I(a[1]), I(a[2]), mp_obj_str_get_str(a[3]), c));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(win_text_obj, 4, win_text);

static mp_obj_t win_blit(size_t n, const mp_obj_t *a, mp_map_t *kw) {   /* blit(x, y, buf, w, h, alpha=False): buf is w*h little-endian 0xAARRGGBB words */
    mp_map_elem_t *al = mp_map_lookup(kw, MP_OBJ_NEW_QSTR(MP_QSTR_alpha), MP_MAP_LOOKUP);
    if (n != 6) mp_raise_TypeError(MP_ERROR_TEXT("blit(x, y, buf, w, h, alpha=False)"));
    int bw = I(a[4]), bh = I(a[5]);
    mp_buffer_info_t b;
    mp_get_buffer_raise(a[3], &b, MP_BUFFER_READ);
    if (bw < 0 || bh < 0 || (size_t)bw * bh * 4 > b.len) mp_raise_ValueError(MP_ERROR_TEXT("buffer too small"));
    (al && mp_obj_is_true(al->value) ? gfx_blit_alpha : gfx_blit)(&W(a[0])->cv, I(a[1]), I(a[2]), b.buf, bw, bh, bw);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(win_blit_obj, 6, win_blit);

static mp_obj_t win_present(mp_obj_t self) { gui_flip(W(self)); return mp_const_none; }
static MP_DEFINE_CONST_FUN_OBJ_1(win_present_obj, win_present);

/* ---- events ---- */
static mp_obj_t event(const struct gui_event *e) {
    static const qstr fields[] = { MP_QSTR_type, MP_QSTR_x, MP_QSTR_y, MP_QSTR_key, MP_QSTR_ch, MP_QSTR_mods, MP_QSTR_button,
                                   MP_QSTR_pressed, MP_QSTR_dz, MP_QSTR_w, MP_QSTR_h };
    static const qstr types[] = { MP_QSTR__question_, MP_QSTR_key, MP_QSTR_mouse_move, MP_QSTR_mouse_down, MP_QSTR_mouse_up,
                                  MP_QSTR_scroll, MP_QSTR_resize, MP_QSTR_close, MP_QSTR_focus, MP_QSTR_mouse_leave, MP_QSTR_drop,
                                  MP_QSTR_theme };
    char ch[3] = { 0 };
    if (e->ch > 0 && e->ch < 0x80) ch[0] = e->ch;
    else if (e->ch >= 0x80 && e->ch < 0x800) { ch[0] = 0xC0 | e->ch >> 6; ch[1] = 0x80 | (e->ch & 63); }
    mp_obj_t it[11] = {
        MP_OBJ_NEW_QSTR(e->type > 0 && e->type <= EV_THEME ? types[e->type] : types[0]),
        MP_OBJ_NEW_SMALL_INT(e->x), MP_OBJ_NEW_SMALL_INT(e->y), MP_OBJ_NEW_SMALL_INT(e->key), mp_obj_new_str_from_cstr(ch),
        MP_OBJ_NEW_SMALL_INT(e->mods), MP_OBJ_NEW_SMALL_INT(e->button), mp_obj_new_bool(e->pressed),
        MP_OBJ_NEW_SMALL_INT(e->dz), MP_OBJ_NEW_SMALL_INT(e->w), MP_OBJ_NEW_SMALL_INT(e->h),
    };
    return mp_obj_new_attrtuple(fields, 11, it);
}

static int next(struct gui_window *w, struct gui_event *e, int ms) {
    for (;;) {
        int r = gui_next_event(w, e, ms);
        if (r >= 0) return r;
        if (errno != EINTR) mp_raise_OSError(errno);
        mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS);
    }
}

static mp_obj_t win_poll(size_t n, const mp_obj_t *a) {              /* one event, waiting up to ms (default 0, < 0 forever) */
    struct gui_event e;
    return next(W(a[0]), &e, n > 1 ? I(a[1]) : 0) ? event(&e) : mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(win_poll_obj, 1, 2, win_poll);

static mp_obj_t win_events(size_t n, const mp_obj_t *a) {            /* all pending events; waits up to ms for the first */
    struct gui_window *w = W(a[0]);
    struct gui_event e;
    mp_obj_t l = mp_obj_new_list(0, NULL);
    for (int ms = n > 1 ? I(a[1]) : 0; next(w, &e, ms); ms = 0) mp_obj_list_append(l, event(&e));
    return l;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(win_events_obj, 1, 2, win_events);

static const mp_rom_map_elem_t win_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_fill), MP_ROM_PTR(&win_fill_obj) },
    { MP_ROM_QSTR(MP_QSTR_rect), MP_ROM_PTR(&win_rect_obj) },
    { MP_ROM_QSTR(MP_QSTR_line), MP_ROM_PTR(&win_line_obj) },
    { MP_ROM_QSTR(MP_QSTR_circle), MP_ROM_PTR(&win_circle_obj) },
    { MP_ROM_QSTR(MP_QSTR_pixel), MP_ROM_PTR(&win_pixel_obj) },
    { MP_ROM_QSTR(MP_QSTR_text), MP_ROM_PTR(&win_text_obj) },
    { MP_ROM_QSTR(MP_QSTR_blit), MP_ROM_PTR(&win_blit_obj) },
    { MP_ROM_QSTR(MP_QSTR_present), MP_ROM_PTR(&win_present_obj) },
    { MP_ROM_QSTR(MP_QSTR_poll), MP_ROM_PTR(&win_poll_obj) },
    { MP_ROM_QSTR(MP_QSTR_events), MP_ROM_PTR(&win_events_obj) },
    { MP_ROM_QSTR(MP_QSTR_fileno), MP_ROM_PTR(&win_fileno_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_title), MP_ROM_PTR(&win_set_title_obj) },
    { MP_ROM_QSTR(MP_QSTR_resize), MP_ROM_PTR(&win_resize_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&win_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&win_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&win_exit_obj) },
};
static MP_DEFINE_CONST_DICT(win_locals, win_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(gui_type_window, MP_QSTR_Window, MP_TYPE_FLAG_NONE,
    make_new, win_make_new, attr, win_attr, locals_dict, &win_locals);

/* ---- module ---- */
static mp_obj_t gui_rgb(size_t n, const mp_obj_t *a) {
    return mp_obj_new_int_from_uint((n == 4 ? (uint32_t)(I(a[3]) & 255) << 24 : 0) | (I(a[0]) & 255) << 16 | (I(a[1]) & 255) << 8 | (I(a[2]) & 255));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gui_rgb_obj, 3, 4, gui_rgb);

static mp_obj_t gui_text_size(size_t n, const mp_obj_t *a) {         /* text_size(s, font="mono") -> (width, height) */
    const struct font *f = font_of(n > 1 ? a[1] : mp_const_none);
    mp_obj_t t[2] = { MP_OBJ_NEW_SMALL_INT(gfx_text_width(f, mp_obj_str_get_str(a[0]))), MP_OBJ_NEW_SMALL_INT(f->height) };
    return mp_obj_new_tuple(2, t);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gui_text_size_obj, 1, 2, gui_text_size);

static mp_obj_t gui_theme(void) {
#define TK(n) { MP_QSTR_##n, offsetof(struct theme, n) }
    static const struct { uint16_t q, off; } tk[] = {
        TK(base), TK(window), TK(raised), TK(overlay), TK(line), TK(line_strong), TK(control), TK(text), TK(text2), TK(text3),
        TK(accent), TK(on_accent), TK(accent_text), TK(accent_line), TK(selection), TK(danger), TK(warning), TK(success), TK(info),
    };
    gui_theme_load();
    mp_obj_t d = mp_obj_new_dict(20);
    for (size_t i = 0; i < MP_ARRAY_SIZE(tk); i++)
        mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(tk[i].q), MP_OBJ_NEW_SMALL_INT(*(color_t *)((char *)&ui_th + tk[i].off) & 0xFFFFFF));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_dark), mp_obj_new_bool(ui_th.dark));
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_0(gui_theme_obj, gui_theme);

#define K(n) { MP_ROM_QSTR(MP_QSTR_##n), MP_ROM_INT(n) }
static const mp_rom_map_elem_t gui_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_gui) },
    { MP_ROM_QSTR(MP_QSTR_Window), MP_ROM_PTR(&gui_type_window) },
    { MP_ROM_QSTR(MP_QSTR_rgb), MP_ROM_PTR(&gui_rgb_obj) },
    { MP_ROM_QSTR(MP_QSTR_text_size), MP_ROM_PTR(&gui_text_size_obj) },
    { MP_ROM_QSTR(MP_QSTR_theme), MP_ROM_PTR(&gui_theme_obj) },
    K(KEY_ESC), K(KEY_BACKSPACE), K(KEY_TAB), K(KEY_ENTER), K(KEY_DELETE), K(KEY_UP), K(KEY_DOWN), K(KEY_LEFT), K(KEY_RIGHT),
    K(KEY_HOME), K(KEY_END), K(KEY_PGUP), K(KEY_PGDN), K(KEY_INSERT), K(KEY_F1), K(KEY_F2), K(KEY_F3), K(KEY_F4), K(KEY_F5),
    K(KEY_F6), K(KEY_F7), K(KEY_F8), K(KEY_F9), K(KEY_F10), K(KEY_F11), K(KEY_F12),
    K(MOD_SHIFT), K(MOD_CTRL), K(MOD_ALT), K(MOD_SUPER),
    { MP_ROM_QSTR(MP_QSTR_BUTTON_LEFT), MP_ROM_INT(1) }, { MP_ROM_QSTR(MP_QSTR_BUTTON_RIGHT), MP_ROM_INT(2) },
    { MP_ROM_QSTR(MP_QSTR_BUTTON_MIDDLE), MP_ROM_INT(4) },
};
static MP_DEFINE_CONST_DICT(gui_globals, gui_globals_table);

const mp_obj_module_t mp_module_gui = { .base = { &mp_type_module }, .globals = (mp_obj_dict_t *)&gui_globals };
MP_REGISTER_MODULE(MP_QSTR_gui, mp_module_gui);
