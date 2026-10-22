/* theme.c - the active theme. /dev/theme reads a struct theme_state and takes text commands
 * ("light", "accent=2979FF", "anim=0", "zoom=2"); /etc/theme holds the same text and is applied at boot.
 * A change repaints the compositor, the terminals and tells every libgui window (EV_THEME). */
#include "wm.h"
#include "vfs.h"
#include "term.h"

struct theme cur_theme;
int theme_anim = 1, theme_zoom = TERM_ZOOM_DEF;
static struct theme_state st;
static bool pend_zoom;

static void commit(int idx, uint32_t accent, int anim, int zoom) {
    cur_theme = *theme_get(idx);
    theme_set_accent(&cur_theme, accent);
    term_set_colors(&cur_theme.term);
    if ((uint32_t)zoom != st.zoom) pend_zoom = true;                /* sticky until the compositor has told the terminals */
    st.size = sizeof st; st.serial++; st.index = idx; st.accent = accent; st.anim = anim; st.zoom = zoom;
    st.th = cur_theme;
    theme_anim = anim; theme_zoom = zoom;
}

/* "NAME accent=RRGGBB|default|PRESET anim=0|1|on|off zoom=0..3", any subset; nothing changes unless all of it parses */
static int parse(const char *s, uint32_t n, bool apply) {
    int idx = st.index, anim = st.anim, zoom = st.zoom;
    uint32_t accent = st.accent;
    char tok[32];
    for (uint32_t i = 0; i < n; ) {
        while (i < n && (s[i] <= ' ')) i++;
        uint32_t k = 0;
        while (i < n && s[i] > ' ') { if (k < sizeof tok - 1) tok[k++] = s[i]; i++; }
        tok[k] = 0;
        if (!k) break;
        if (!strncmp(tok, "accent=", 7)) {
            const char *v = tok + 7;
            int a = theme_accent_find(v);
            if (!strcmp(v, "default")) accent = 0;
            else if (a >= 0) accent = theme_accents[a].rgb;
            else {
                uint32_t c = 0; int d = 0;
                for (; v[d]; d++) {
                    int h = v[d] >= '0' && v[d] <= '9' ? v[d] - '0' : (v[d] | 32) >= 'a' && (v[d] | 32) <= 'f' ? (v[d] | 32) - 'a' + 10 : -1;
                    if (h < 0) return -EINVAL;
                    c = c << 4 | h;
                }
                if (d != 6) return -EINVAL;
                accent = 0xFF000000u | c;
            }
        } else if (!strncmp(tok, "anim=", 5)) {
            anim = !strcmp(tok + 5, "1") || !strcmp(tok + 5, "on");
            if (!anim && strcmp(tok + 5, "0") && strcmp(tok + 5, "off")) return -EINVAL;
        } else if (!strncmp(tok, "zoom=", 5)) {
            zoom = tok[5] - '0';
            if (zoom < 0 || zoom >= TERM_ZOOMS || tok[6]) return -EINVAL;
        } else if ((idx = theme_find(tok)) < 0) return -EINVAL;
    }
    if (apply) { commit(idx, accent, anim, zoom); wm_theme_changed(); }
    return 0;
}

static int th_read(struct vnode *v, uint32_t off, void *buf, uint32_t n) {
    if (off >= sizeof st) return 0;
    n = MIN(n, sizeof st - off);
    memcpy(buf, (char *)&st + off, n);
    return n;
}

static int th_write(struct vnode *v, uint32_t off, const void *buf, uint32_t n) {
    int r = parse(buf, n, false);
    if (r < 0) return r;
    parse(buf, n, true);
    return n;
}

static int th_truncate(struct vnode *v, uint32_t len) { return 0; }
static int th_poll(struct vnode *v, int ev) { return ev & (POLLIN | POLLOUT); }
const struct vnode_ops theme_ops = { .read = th_read, .write = th_write, .truncate = th_truncate, .poll = th_poll };

/* the zoom step changed with the last switch (terminals refit only then, so a Ctrl+= in one window survives a recolour) */
bool theme_zoom_changed(void) { bool z = pend_zoom; pend_zoom = false; return z; }

void theme_init(void) {
    commit(0, 0, 1, TERM_ZOOM_DEF);
    void *d; uint32_t n;
    if (vfs_read_all("/etc/theme", &d, &n) == 0) {
        if (parse(d, n, false) == 0) parse(d, n, true);
        kfree(d);
    }
    pend_zoom = false;
}
