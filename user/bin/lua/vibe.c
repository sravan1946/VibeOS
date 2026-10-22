/* vibe.c - the VibeOS side of /bin/lua: `vibe` (system calls) and `gui` (windows) modules,
 * and vibe_openlibs(), which lua.c calls instead of luaL_openlibs (see vibe_luaconf.h). */
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <syscall.h>
#include <gui.h>

/* ---------------- vibe ---------------- */

static int v_sysinfo(lua_State *L) {
    struct sysinfo si;
    if (sysinfo(&si) < 0) return luaL_fileresult(L, 0, "sysinfo");
    lua_createtable(L, 0, 9);
#define F(n) (lua_pushinteger(L, si.n), lua_setfield(L, -2, #n))
    F(uptime_ms); F(mem_total_kb); F(mem_free_kb); F(heap_total_kb); F(heap_used_kb);
    F(nprocs); F(idle_ms); F(screen_w); F(screen_h);
#undef F
    return 1;
}

static int v_ps(lua_State *L) {
    static const char *const st[] = { "running", "ready", "blocked", "zombie" };
    struct procinfo p[128];
    int n = proclist(p, 128);
    if (n < 0) return luaL_fileresult(L, 0, "proclist");
    lua_createtable(L, n, 0);
    for (int i = 0; i < n; i++) {
        lua_createtable(L, 0, 6);
        lua_pushinteger(L, p[i].pid); lua_setfield(L, -2, "pid");
        lua_pushinteger(L, p[i].ppid); lua_setfield(L, -2, "ppid");
        lua_pushstring(L, p[i].state >= 0 && p[i].state < 4 ? st[p[i].state] : "?"); lua_setfield(L, -2, "state");
        lua_pushinteger(L, p[i].mem_kb); lua_setfield(L, -2, "mem_kb");
        lua_pushinteger(L, p[i].cpu_ms); lua_setfield(L, -2, "cpu_ms");
        lua_pushstring(L, p[i].name); lua_setfield(L, -2, "name");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int v_beep(lua_State *L) {                 /* vibe.beep(hz, ms): returns once the tone has been queued */
    lua_Integer hz = luaL_checkinteger(L, 1), ms = luaL_optinteger(L, 2, 200);
    beep((uint32_t)hz, (uint32_t)ms);
    return 0;
}

static int v_sleep(lua_State *L) {
    lua_Integer ms = luaL_checkinteger(L, 1);
    if (ms > 0) __syscall1(SYS_sleep, (int)ms);
    return 0;
}

static int v_uptime(lua_State *L) {               /* seconds since boot, millisecond resolution */
    lua_pushnumber(L, uptime_ms() / 1000.0);
    return 1;
}

static int v_listdir(lua_State *L) {              /* the os library cannot list a directory */
    const char *path = luaL_optstring(L, 1, ".");
    DIR *d = opendir(path);
    if (!d) return luaL_fileresult(L, 0, path);
    lua_newtable(L);
    int i = 1;
    for (struct dirent *e; (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        lua_pushstring(L, e->d_name);
        lua_rawseti(L, -2, i++);
    }
    closedir(d);
    return 1;
}

static int v_isdir(lua_State *L) {
    struct stat st;
    lua_pushboolean(L, stat(luaL_checkstring(L, 1), &st) == 0 && S_ISDIR(st.st_mode));
    return 1;
}

static int v_mkdir(lua_State *L) {
    const char *p = luaL_checkstring(L, 1);
    return luaL_fileresult(L, mkdir(p, 0755) == 0, p);
}

static int v_chdir(lua_State *L) {
    const char *p = luaL_checkstring(L, 1);
    return luaL_fileresult(L, chdir(p) == 0, p);
}

static int v_getcwd(lua_State *L) {
    char b[PATH_MAX];
    if (!getcwd(b, sizeof b)) return luaL_fileresult(L, 0, "getcwd");
    lua_pushstring(L, b);
    return 1;
}

static const luaL_Reg vibe_lib[] = {
    { "sysinfo", v_sysinfo }, { "ps", v_ps }, { "beep", v_beep }, { "sleep", v_sleep }, { "uptime", v_uptime },
    { "listdir", v_listdir }, { "isdir", v_isdir }, { "mkdir", v_mkdir }, { "chdir", v_chdir }, { "getcwd", v_getcwd },
    { NULL, NULL }
};

static int luaopen_vibe(lua_State *L) {
    luaL_newlib(L, vibe_lib);
    return 1;
}

/* ---------------- gui ----------------
 * w = gui.open(title, width, height)     colours are 0xRRGGBB integers (or gui.rgb(r, g, b))
 * w:clear(c)  w:pixel(x, y, c)  w:fill(x, y, w, h, c)  w:rect(x, y, w, h, c)  w:line(x0, y0, x1, y1, c)
 * w:circle(cx, cy, r, c)  w:text(x, y, s [, c [, font]]) -> advance in pixels  w:flip()  w:size()
 * w:poll([timeout_ms]) -> event table or nil   w:title(s)   w:close() */

#define GUI_MT "vibe.window"

static struct gui_window *win_of(lua_State *L, int i) {
    struct gui_window **pw = luaL_checkudata(L, i, GUI_MT);
    if (!*pw) luaL_error(L, "window is closed");
    return *pw;
}

static color_t col(lua_State *L, int i) {
    lua_Integer c = luaL_checkinteger(L, i);
    return c > 0xFFFFFF ? (color_t)c : (color_t)(0xFF000000u | (uint32_t)c);
}

static int g_open(lua_State *L) {
    const char *title = luaL_checkstring(L, 1);
    int w = (int)luaL_optinteger(L, 2, 400), h = (int)luaL_optinteger(L, 3, 300);
    struct gui_window **pw = lua_newuserdatauv(L, sizeof *pw, 0);
    *pw = NULL;
    luaL_setmetatable(L, GUI_MT);
    *pw = gui_window_create(title, w, h, 0);
    if (!*pw) return luaL_fileresult(L, 0, "gui.open");
    return 1;
}

static int g_rgb(lua_State *L) {
    lua_pushinteger(L, ((luaL_checkinteger(L, 1) & 255) << 16) | ((luaL_checkinteger(L, 2) & 255) << 8) | (luaL_checkinteger(L, 3) & 255));
    return 1;
}

static int w_gc(lua_State *L) {
    struct gui_window **pw = luaL_checkudata(L, 1, GUI_MT);
    if (*pw) { gui_window_destroy(*pw); *pw = NULL; }
    return 0;
}

static int w_clear(lua_State *L) {
    struct gui_window *w = win_of(L, 1);
    gfx_fill(&w->cv, 0, 0, w->w, w->h, col(L, 2));
    return 0;
}

static int w_pixel(lua_State *L) {
    gfx_pixel(&win_of(L, 1)->cv, (int)luaL_checkinteger(L, 2), (int)luaL_checkinteger(L, 3), col(L, 4));
    return 0;
}

static int w_fill(lua_State *L) {
    gfx_fill(&win_of(L, 1)->cv, (int)luaL_checkinteger(L, 2), (int)luaL_checkinteger(L, 3),
             (int)luaL_checkinteger(L, 4), (int)luaL_checkinteger(L, 5), col(L, 6));
    return 0;
}

static int w_rect(lua_State *L) {
    gfx_rect(&win_of(L, 1)->cv, (int)luaL_checkinteger(L, 2), (int)luaL_checkinteger(L, 3),
             (int)luaL_checkinteger(L, 4), (int)luaL_checkinteger(L, 5), col(L, 6));
    return 0;
}

static int w_line(lua_State *L) {
    gfx_line(&win_of(L, 1)->cv, (int)luaL_checkinteger(L, 2), (int)luaL_checkinteger(L, 3),
             (int)luaL_checkinteger(L, 4), (int)luaL_checkinteger(L, 5), col(L, 6));
    return 0;
}

static int w_circle(lua_State *L) {
    gfx_circle_fill(&win_of(L, 1)->cv, (int)luaL_checkinteger(L, 2), (int)luaL_checkinteger(L, 3),
                    (int)luaL_checkinteger(L, 4), col(L, 5));
    return 0;
}

static int w_text(lua_State *L) {
    static const char *const names[] = { "mono", "mono_bold", "ui", "ui_bold", "title", "big", NULL };
    static const struct font *const fonts[] = { &font_mono, &font_mono_bold, &font_ui, &font_ui_bold, &font_title, &font_big };
    struct gui_window *w = win_of(L, 1);
    const char *s = luaL_checkstring(L, 4);
    color_t c = lua_isnoneornil(L, 5) ? RGB(255, 255, 255) : col(L, 5);
    const struct font *f = fonts[luaL_checkoption(L, 6, "mono", names)];
    lua_pushinteger(L, gfx_text(&w->cv, f, (int)luaL_checkinteger(L, 2), (int)luaL_checkinteger(L, 3), s, c));
    return 1;
}

static int w_flip(lua_State *L) {
    gui_flip(win_of(L, 1));
    return 0;
}

static int w_size(lua_State *L) {
    struct gui_window *w = win_of(L, 1);
    lua_pushinteger(L, w->w);
    lua_pushinteger(L, w->h);
    return 2;
}

static int w_title(lua_State *L) {
    gui_set_title(win_of(L, 1), luaL_checkstring(L, 2));
    return 0;
}

static void setint(lua_State *L, const char *k, lua_Integer v) { lua_pushinteger(L, v); lua_setfield(L, -2, k); }

static int w_poll(lua_State *L) {
    struct gui_window *w = win_of(L, 1);
    struct gui_event ev;
    int r = gui_next_event(w, &ev, (int)luaL_optinteger(L, 2, 0));
    if (r < 0) return luaL_fileresult(L, 0, "gui.poll");
    if (r == 0) { lua_pushnil(L); return 1; }
    static const char *const types[] = { "?", "key", "mouse_move", "mouse_down", "mouse_up", "scroll", "resize", "close", "focus", "mouse_leave" };
    lua_createtable(L, 0, 8);
    lua_pushstring(L, ev.type > 0 && ev.type < 10 ? types[ev.type] : "?");
    lua_setfield(L, -2, "type");
    switch (ev.type) {
    case EV_KEY:
        setint(L, "key", ev.key); setint(L, "mods", ev.mods);
        lua_pushboolean(L, ev.pressed); lua_setfield(L, -2, "pressed");
        if (ev.ch > 0 && ev.ch < 256) { char c = (char)ev.ch; lua_pushlstring(L, &c, 1); lua_setfield(L, -2, "ch"); }
        break;
    case EV_MOUSE_MOVE: setint(L, "x", ev.x); setint(L, "y", ev.y); setint(L, "buttons", ev.button); break;
    case EV_MOUSE_DOWN: case EV_MOUSE_UP: setint(L, "x", ev.x); setint(L, "y", ev.y); setint(L, "button", ev.button); break;
    case EV_SCROLL: setint(L, "x", ev.x); setint(L, "y", ev.y); setint(L, "dz", ev.dz); break;
    case EV_RESIZE: setint(L, "w", ev.w); setint(L, "h", ev.h); break;
    case EV_FOCUS: lua_pushboolean(L, ev.pressed); lua_setfield(L, -2, "focused"); break;
    }
    return 1;
}

static const luaL_Reg win_methods[] = {
    { "clear", w_clear }, { "pixel", w_pixel }, { "fill", w_fill }, { "rect", w_rect }, { "line", w_line },
    { "circle", w_circle }, { "text", w_text }, { "flip", w_flip }, { "size", w_size }, { "poll", w_poll },
    { "title", w_title }, { "close", w_gc }, { "__gc", w_gc }, { "__close", w_gc },
    { NULL, NULL }
};

static const luaL_Reg gui_lib[] = { { "open", g_open }, { "rgb", g_rgb }, { NULL, NULL } };

static int luaopen_gui(lua_State *L) {
    luaL_newmetatable(L, GUI_MT);
    luaL_setfuncs(L, win_methods, 0);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
    luaL_newlib(L, gui_lib);
    return 1;
}

void vibe_openlibs(lua_State *L) {
    luaL_openlibs(L);
    luaL_requiref(L, "vibe", luaopen_vibe, 1);
    luaL_requiref(L, "gui", luaopen_gui, 1);
    lua_pop(L, 2);
}
