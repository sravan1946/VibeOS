/* cssi.h - internals shared by css.c (values, declarations, sheets, cascade) and cssel.c (selectors). Not for other modules. */
#pragma once
#include "browser.h"

/* interned strings: ids are offsets into one pool (0 = ""); the pool only grows until css_reset() decides to start over */
int  intern(const char *s, int n);
extern char *SP;
#define STR(i) (SP + (i))
uint32_t hash_str(const char *s);

/* ---- selectors ---- */
enum { T_TAG, T_ID, T_CLASS, T_ATTR, T_PC, T_NOT, T_IS, T_HAS, T_NTH };
enum { PC_FIRSTC, PC_LASTC, PC_ONLYC, PC_FIRSTT, PC_LASTT, PC_ONLYT, PC_EMPTY, PC_ROOT, PC_LINK, PC_HOVER, PC_FOCUS, PC_ACTIVE, PC_CHECKED,
       PC_DISABLED, PC_ENABLED, PC_FOCUSWITHIN, PC_REQUIRED, PC_OPTIONAL, PC_READONLY, PC_READWRITE, PC_NEVER, PC_ALWAYS, PC_LANG };
struct simple { uint8_t t, op, ci; int a, b; };       /* T_ATTR: a name, b value, op; T_PC: a PC_*; T_NOT/IS/HAS: a first sel, b count; T_NTH: a, b = An+B, op: 0 child 1 last-child 2 of-type 3 last-of-type */
struct comp { int s0; uint8_t ns, comb; };            /* comb: how this compound relates to the one on its left: ' ' '>' '+' '~' (0 for the first) */
#define BLW 8                                       /* bloom filter words: 256 bits, enough for the ~60 keys of a deep ancestor chain */
struct sel { int c0; uint8_t nc, pe; uint32_t spec; uint32_t mask[BLW], rmask[BLW]; };   /* pe: PE_*; mask: bloom bits the ancestors must have; rmask: ...the subject itself */
extern struct simple *SI; extern struct comp *CO; extern struct sel *SE;
extern int nsi, nco, nse;
extern const struct css_dom *DOM;
/* parse one complex selector s[0..n) (no top-level comma) into *out; 1 = ok */
int  sel_parse(const char *s, int n, struct sel *out);
int  sel_push(const struct sel *s);
void sel_mark(int *a, int *b, int *c);
void sel_trunc(int a, int b, int c);
int  sel_match(const struct sel *s, int node);

void sel_key(const struct sel *s, int *kind, uint32_t *h);   /* bucket: kind 1 id, 2 class, 3 tag, 0 any */
void bloom_add(uint32_t *b, uint32_t h);
int  bloom_has(const uint32_t *b, const uint32_t *m);
uint32_t bloom_keyn(int kind, const char *s, int n);
uint32_t bloom_key(int kind, const char *s);   /* kind: 1 tag, 2 id, 3 class */
extern int attrs_used;
