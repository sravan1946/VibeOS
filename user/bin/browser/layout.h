/* layout.h - internal to the page engine: dom.c (HTML -> node tree), layout.c (style, boxes, layout, paint), html.c (the
 * display list's queries and drawing). Nothing outside user/bin/browser/ includes this. */
#pragma once
#include "browser.h"

/* ---- dom.c ---- */
enum { T_NONE, T_HTML, T_HEAD, T_BODY, T_H1, T_H2, T_H3, T_H4, T_H5, T_H6, T_P, T_DIV, T_CENTER, T_UL, T_OL, T_LI, T_DL, T_DT,
       T_DD, T_BQ, T_PRE, T_CODE, T_B, T_I, T_U, T_S, T_A, T_FONT, T_BR, T_HR, T_IMG, T_TABLE, T_TR, T_TD, T_TH, T_CAP, T_TGROUP,
       T_FORM, T_INPUT, T_TEXTAREA, T_SELECT, T_OPTION, T_BUTTON, T_SKIP, T_SUP, T_SUB, T_SMALL, T_BIG, T_LEGEND, T_COL, T_SVG };
struct dnode { int js, par, first, last, next, prev; uint8_t type, tag; short na; int a0; uint32_t nm; int tn; };   /* type 1 element, 3 text, 9 root; nm: pool offset of the name / text */
struct dattr { uint32_t n, v; };
struct dom { int quirks, js; struct dnode *n; int nn, cap; struct dattr *a; int na, acap; char *pool; size_t np, pcap; };
extern struct dom HD;
#define DN(i) (HD.n[i])
static inline const char *hd_str(uint32_t off) { return HD.pool + off; }
const char *hd_attr(int n, const char *name);           /* NULL when absent */
int  hd_parse(const char *src, size_t n, const char *base, struct doc *d);   /* builds HD (node 0 = root), feeds <style>/<link> to css.c, sets d->title */
void hd_free(void);
int  hd_ent(const char *s, int n, char *o, int *used);  /* s[0] == '&': UTF-8 written to o (0 = not an entity); &nbsp; stays U+00A0 */

/* ---- html.c ---- */
enum { F_UI, F_UIB, F_TITLE, F_MONO, F_MONOB };
extern const struct font *FT[];
int font_scaled(int px);                                   /* font index for page text of px (>= 22) CSS px */
uint32_t hd_pool_add(const char *s, size_t n);
void hd_plain(const char *src, size_t n);
int  hd_from_js(struct doc *d, const char *base);   /* the page's tree from js.c's accessors (pages with scripts) */
