/* gfx.h - 32bpp software rendering shared by the kernel and userland */
#pragma once
#include <stdint.h>
#include "font.h"

typedef uint32_t color_t;                 /* 0xAARRGGBB */
#define RGB(r, g, b)     (0xFF000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define RGBA(r, g, b, a) (((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define ALPHA(c)         ((c) >> 24)

struct canvas {
    uint32_t *px;
    int w, h, stride;                     /* stride in pixels */
    int cx0, cy0, cx1, cy1;               /* clip rect, [x0,x1) */
};

struct rect { int x, y, w, h; };

void canvas_init(struct canvas *c, uint32_t *px, int w, int h, int stride);
void canvas_clip(struct canvas *c, int x, int y, int w, int h);   /* intersect */
void canvas_noclip(struct canvas *c);

color_t blend(color_t dst, color_t src);          /* src over dst, uses src alpha */
color_t mix(color_t a, color_t b, int t);         /* t in 0..256 */

void gfx_pixel(struct canvas *c, int x, int y, color_t col);
void gfx_fill(struct canvas *c, int x, int y, int w, int h, color_t col);      /* blends if alpha<255 */
void gfx_rect(struct canvas *c, int x, int y, int w, int h, color_t col);      /* 1px outline */
static inline void gfx_hline(struct canvas *c, int x, int y, int w, color_t col) { gfx_fill(c, x, y, w, 1, col); }   /* crisp 1px */
static inline void gfx_vline(struct canvas *c, int x, int y, int h, color_t col) { gfx_fill(c, x, y, 1, h, col); }
/* rounded rects, anti-aliased corners. radii pack the four corners; RADII(r, r, r, r) is a plain rounded rect */
#define RADII(tl, tr, br, bl) ((uint32_t)(tl) | (uint32_t)(tr) << 8 | (uint32_t)(br) << 16 | (uint32_t)(bl) << 24)
void gfx_rrect_fill(struct canvas *c, int x, int y, int w, int h, uint32_t radii, color_t col);
void gfx_rrect_ring(struct canvas *c, int x, int y, int w, int h, uint32_t radii, int t, color_t col);   /* t px, inside the edge */
void gfx_round_fill(struct canvas *c, int x, int y, int w, int h, int r, color_t col);
void gfx_round_rect(struct canvas *c, int x, int y, int w, int h, int r, color_t col);    /* 1px inset ring */
void gfx_ring_out(struct canvas *c, int x, int y, int w, int h, int r, color_t col);      /* 1px ring just outside, radius r+1 */
void gfx_focus_ring(struct canvas *c, int x, int y, int w, int h, int r, color_t col);    /* FOCUS_W px, FOCUS_GAP outside, concentric */
void gfx_vgradient(struct canvas *c, int x, int y, int w, int h, color_t top, color_t bot);
/* baked 9-slice drop shadow under a w x h rect of radius r: black, offset dy, blur (sigma blur/2), peak alpha 0..255.
 * Cached per (r, blur); a frame costs one blend per shadow pixel outside the caster */
void gfx_shadow9(struct canvas *c, int x, int y, int w, int h, int r, int dy, int blur, int alpha);
void gfx_elev(struct canvas *c, int x, int y, int w, int h, int r, int level, int alpha);   /* E1..E4 (design.h), alpha = theme elev[level] */
/* an A8 coverage mask (icons, glyphs) tinted with col */
void gfx_mask(struct canvas *c, int x, int y, const uint8_t *a, int w, int h, int stride, color_t col);
void gfx_circle_fill(struct canvas *c, int cx, int cy, int r, color_t col);
void gfx_line(struct canvas *c, int x0, int y0, int x1, int y1, color_t col);
void gfx_line_aa(struct canvas *c, float x0, float y0, float x1, float y1, float width, color_t col);
void gfx_blit(struct canvas *c, int x, int y, const uint32_t *src, int w, int h, int stride);
void gfx_blit_alpha(struct canvas *c, int x, int y, const uint32_t *src, int w, int h, int stride);
void gfx_blit_premul(struct canvas *c, int x, int y, const uint32_t *src, int w, int h, int stride);   /* premultiplied ARGB (app/file icons) */
void gfx_blit_scaled(struct canvas *c, int x, int y, int dw, int dh, const uint32_t *src, int sw, int sh, int stride);

int  gfx_char(struct canvas *c, const struct font *f, int x, int y, uint32_t cp, color_t col);  /* codepoint; returns advance */
const struct glyph *font_glyph(const struct font *f, uint32_t cp);   /* '?' if the font lacks it */
int  utf8_decode(const char *s, int n, uint32_t *cp);                 /* bytes consumed, >= 1 */
/* proportional faces use a 1/64 px pen (glyph adv64), monospaced ones whole-pixel cells; returns the advance */
int  gfx_text(struct canvas *c, const struct font *f, int x, int y, const char *s, color_t col);
/* text is UTF-8; n limits bytes */
int  gfx_text_n(struct canvas *c, const struct font *f, int x, int y, const char *s, int n, color_t col);
int  gfx_text_width(const struct font *f, const char *s);
int  gfx_text_width_n(const struct font *f, const char *s, int n);
void gfx_text_center(struct canvas *c, const struct font *f, int x, int y, int w, const char *s, color_t col);
/* s cut to maxw px with an ellipsis; returns the width drawn */
int  gfx_text_ell(struct canvas *c, const struct font *f, int x, int y, int maxw, const char *s, color_t col);
