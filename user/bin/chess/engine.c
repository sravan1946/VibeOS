/* engine.c - move generation, make, SAN, status and the searcher; see engine.h */
#include "engine.h"
#include <string.h>

static const signed char KN[8][2] = { {1,2},{2,1},{2,-1},{1,-2},{-1,-2},{-2,-1},{-2,1},{-1,2} };
static const signed char KG[8][2] = { {1,0},{1,1},{0,1},{-1,1},{-1,0},{-1,-1},{0,-1},{1,-1} };
static const signed char DG[4][2] = { {1,1},{1,-1},{-1,1},{-1,-1} };
static const signed char OR[4][2] = { {1,0},{-1,0},{0,1},{0,-1} };

static int sgn(int side) { return side == WHITE ? 1 : -1; }
static int own(int piece, int side) { return piece != 0 && (piece > 0) == (side == WHITE); }

void cp_start(cpos *p) {
    static const int8_t back[8] = { R_, N_, B_, Q_, K_, B_, N_, R_ };
    memset(p, 0, sizeof *p);
    for (int f = 0; f < 8; f++) {
        p->b[f] = back[f]; p->b[8 + f] = P_;
        p->b[48 + f] = -P_; p->b[56 + f] = -back[f];
    }
    p->castle = 15; p->ep = -1; p->full = 1;
}

/* is `sq` attacked by a piece of side `by` */
static int attacked(const cpos *p, int sq, int by) {
    int x = sq & 7, y = sq >> 3, s = sgn(by);
    int py = y - s;                                    /* pawns attack forward, so they sit one rank behind */
    if (py >= 0 && py < 8) for (int d = -1; d <= 1; d += 2)
        if (x + d >= 0 && x + d < 8 && p->b[py * 8 + x + d] == s * P_) return 1;
    for (int i = 0; i < 8; i++) {
        int nx = x + KN[i][0], ny = y + KN[i][1];
        if (nx >= 0 && nx < 8 && ny >= 0 && ny < 8 && p->b[ny * 8 + nx] == s * N_) return 1;
        nx = x + KG[i][0]; ny = y + KG[i][1];
        if (nx >= 0 && nx < 8 && ny >= 0 && ny < 8 && p->b[ny * 8 + nx] == s * K_) return 1;
    }
    for (int i = 0; i < 8; i++) {                      /* 0-3 diagonals, 4-7 orthogonals */
        const signed char *d = i < 4 ? DG[i] : OR[i - 4];
        for (int nx = x + d[0], ny = y + d[1]; nx >= 0 && nx < 8 && ny >= 0 && ny < 8; nx += d[0], ny += d[1]) {
            int q = p->b[ny * 8 + nx];
            if (!q) continue;
            if (q * s > 0 && (q * s == Q_ || q * s == (i < 4 ? B_ : R_))) return 1;
            break;
        }
    }
    return 0;
}

int cp_in_check(const cpos *p, int side) {
    for (int i = 0; i < 64; i++) if (p->b[i] == sgn(side) * K_) return attacked(p, i, !side);
    return 0;
}

static int add(cmove *o, int n, int from, int to, int promo) {
    o[n].from = from; o[n].to = to; o[n].promo = promo;
    return n + 1;
}

/* pseudo-legal moves (may leave the king in check); caps_only keeps captures and promotions for quiescence */
static int gen(const cpos *p, cmove *o, int caps_only) {
    int n = 0, side = p->side, s = sgn(side);
    for (int from = 0; from < 64; from++) {
        int pc = p->b[from];
        if (!own(pc, side)) continue;
        int x = from & 7, y = from >> 3, t = pc * s;
        if (t == P_) {
            int ny = y + s, last = ny == 0 || ny == 7;
            if (ny < 0 || ny > 7) continue;
            if (!p->b[ny * 8 + x] && (!caps_only || last)) {
                int to = ny * 8 + x;
                if (last) for (int pr = Q_; pr >= N_; pr--) n = add(o, n, from, to, pr);
                else {
                    n = add(o, n, from, to, 0);
                    if (y == (side == WHITE ? 1 : 6) && !p->b[(y + 2 * s) * 8 + x]) n = add(o, n, from, (y + 2 * s) * 8 + x, 0);
                }
            }
            for (int d = -1; d <= 1; d += 2) {
                int nx = x + d;
                if (nx < 0 || nx > 7) continue;
                int to = ny * 8 + nx, q = p->b[to];
                if ((q && !own(q, side)) || to == p->ep) {
                    if (last) for (int pr = Q_; pr >= N_; pr--) n = add(o, n, from, to, pr);
                    else n = add(o, n, from, to, 0);
                }
            }
        } else if (t == N_ || t == K_) {
            for (int i = 0; i < 8; i++) {
                int nx = x + (t == N_ ? KN[i][0] : KG[i][0]), ny = y + (t == N_ ? KN[i][1] : KG[i][1]);
                if (nx < 0 || nx > 7 || ny < 0 || ny > 7) continue;
                int q = p->b[ny * 8 + nx];
                if (own(q, side) || (caps_only && !q)) continue;
                n = add(o, n, from, ny * 8 + nx, 0);
            }
            if (t == K_ && !caps_only && x == 4 && y == (side == WHITE ? 0 : 7) && !attacked(p, from, !side)) {
                int ks = side == WHITE ? CASTLE_WK : CASTLE_BK, qs = side == WHITE ? CASTLE_WQ : CASTLE_BQ, r = y * 8;
                if ((p->castle & ks) && p->b[r + 7] == s * R_ && !p->b[r + 5] && !p->b[r + 6] && !attacked(p, r + 5, !side))
                    n = add(o, n, from, r + 6, 0);
                if ((p->castle & qs) && p->b[r] == s * R_ && !p->b[r + 1] && !p->b[r + 2] && !p->b[r + 3] && !attacked(p, r + 3, !side))
                    n = add(o, n, from, r + 2, 0);
            }
        } else {                                           /* sliders: bishop 0-3, rook 4-7, queen all */
            for (int i = t == R_ ? 4 : 0; i < (t == B_ ? 4 : 8); i++) {
                const signed char *d = i < 4 ? DG[i] : OR[i - 4];
                for (int nx = x + d[0], ny = y + d[1]; nx >= 0 && nx < 8 && ny >= 0 && ny < 8; nx += d[0], ny += d[1]) {
                    int q = p->b[ny * 8 + nx];
                    if (own(q, side)) break;
                    if (!caps_only || q) n = add(o, n, from, ny * 8 + nx, 0);
                    if (q) break;
                }
            }
        }
    }
    return n;
}

void cp_make(const cpos *p, cmove m, cpos *o) {
    *o = *p;
    int pc = o->b[m.from], t = pc < 0 ? -pc : pc, s = sgn(p->side), cap = o->b[m.to];
    o->b[m.to] = m.promo ? s * m.promo : pc;
    o->b[m.from] = 0;
    if (t == P_ && m.to == p->ep) { o->b[m.to - 8 * s] = 0; cap = 1; }          /* en passant */
    o->ep = -1;
    if (t == P_ && (m.to - m.from == 16 || m.from - m.to == 16)) o->ep = (m.from + m.to) / 2;
    if (t == K_) {
        if (m.to - m.from == 2) { o->b[m.from + 1] = o->b[m.from + 3]; o->b[m.from + 3] = 0; }
        if (m.from - m.to == 2) { o->b[m.from - 1] = o->b[m.from - 4]; o->b[m.from - 4] = 0; }
        o->castle &= p->side == WHITE ? ~(CASTLE_WK | CASTLE_WQ) : ~(CASTLE_BK | CASTLE_BQ);
    }
    static const struct { int sq, mask; } rk[4] = { {0, CASTLE_WQ}, {7, CASTLE_WK}, {56, CASTLE_BQ}, {63, CASTLE_BK} };
    for (int i = 0; i < 4; i++) if (m.from == rk[i].sq || m.to == rk[i].sq) o->castle &= ~rk[i].mask;
    o->half = (t == P_ || cap) ? 0 : p->half + 1;
    if (p->side == BLACK) o->full++;
    o->side = !p->side;
}

int cp_moves(const cpos *p, cmove *out) {
    cmove tmp[256];
    int n = gen(p, tmp, 0), k = 0;
    for (int i = 0; i < n; i++) {
        cpos q;
        cp_make(p, tmp[i], &q);
        if (!cp_in_check(&q, p->side)) out[k++] = tmp[i];
    }
    return k;
}

uint64_t cp_perft(const cpos *p, int depth) {
    cmove ms[256];
    int n = cp_moves(p, ms);
    if (depth <= 1) return n;
    uint64_t t = 0;
    for (int i = 0; i < n; i++) { cpos q; cp_make(p, ms[i], &q); t += cp_perft(&q, depth - 1); }
    return t;
}

static int insufficient(const cpos *p) {
    int minors = 0, bishops_w = 0, bishops_b = 0;
    for (int i = 0; i < 64; i++) {
        int t = p->b[i] < 0 ? -p->b[i] : p->b[i];
        if (!t || t == K_) continue;
        if (t == P_ || t == R_ || t == Q_) return 0;
        minors++;
        if (t == B_) { if (((i & 7) + (i >> 3)) & 1) bishops_b++; else bishops_w++; }
    }
    if (minors <= 1) return 1;
    return minors == bishops_w + bishops_b && (bishops_w == 0 || bishops_b == 0);   /* bishops all on one colour */
}

int cp_status(const cpos *p) {
    cmove ms[256];
    if (!cp_moves(p, ms)) return cp_in_check(p, p->side) ? ST_MATE : ST_STALEMATE;
    if (insufficient(p)) return ST_INSUFFICIENT;
    return p->half >= 100 ? ST_FIFTY : ST_PLAY;
}

/* ponytail: the en passant square is ignored, so a position after a double push can count as a repeat one move early */
int cp_same(const cpos *a, const cpos *b) {
    return a->side == b->side && a->castle == b->castle && !memcmp(a->b, b->b, 64);
}

void cp_san(const cpos *p, cmove m, char *out) {
    int pc = p->b[m.from], t = pc < 0 ? -pc : pc, n = 0, cap = p->b[m.to] != 0 || (t == P_ && m.to == p->ep);
    if (t == K_ && (m.to - m.from == 2 || m.from - m.to == 2)) {
        n = m.to > m.from ? (memcpy(out, "O-O", 3), 3) : (memcpy(out, "O-O-O", 5), 5);
    } else {
        if (t != P_) {
            out[n++] = " PNBRQK"[t];
            cmove ms[256];
            int k = cp_moves(p, ms), same = 0, sf = 0, sr = 0;     /* disambiguate: file, else rank, else both */
            for (int i = 0; i < k; i++)
                if (ms[i].to == m.to && ms[i].from != m.from && p->b[ms[i].from] == pc) {
                    same = 1;
                    if ((ms[i].from & 7) == (m.from & 7)) sf = 1;
                    if ((ms[i].from >> 3) == (m.from >> 3)) sr = 1;
                }
            if (same) {
                if (!sf) out[n++] = 'a' + (m.from & 7);
                else if (!sr) out[n++] = '1' + (m.from >> 3);
                else { out[n++] = 'a' + (m.from & 7); out[n++] = '1' + (m.from >> 3); }
            }
        } else if (cap) out[n++] = 'a' + (m.from & 7);
        if (cap) out[n++] = 'x';
        out[n++] = 'a' + (m.to & 7);
        out[n++] = '1' + (m.to >> 3);
        if (m.promo) { out[n++] = '='; out[n++] = " PNBRQK"[m.promo]; }
    }
    cpos q;
    cp_make(p, m, &q);
    if (cp_status(&q) == ST_MATE) out[n++] = '#';
    else if (cp_in_check(&q, q.side)) out[n++] = '+';
    out[n] = 0;
}

/* ---- search ---- */
uint32_t (*engine_clock)(void);

static const int VAL[7] = { 0, 100, 320, 330, 500, 900, 0 };
/* piece-square tables from white's view with rank 8 first (the "simplified evaluation function") */
static const int8_t PST[7][64] = {
    {0},
    { 0,0,0,0,0,0,0,0, 50,50,50,50,50,50,50,50, 10,10,20,30,30,20,10,10, 5,5,10,25,25,10,5,5,
      0,0,0,20,20,0,0,0, 5,-5,-10,0,0,-10,-5,5, 5,10,10,-20,-20,10,10,5, 0,0,0,0,0,0,0,0 },
    { -50,-40,-30,-30,-30,-30,-40,-50, -40,-20,0,0,0,0,-20,-40, -30,0,10,15,15,10,0,-30, -30,5,15,20,20,15,5,-30,
      -30,0,15,20,20,15,0,-30, -30,5,10,15,15,10,5,-30, -40,-20,0,5,5,0,-20,-40, -50,-40,-30,-30,-30,-30,-40,-50 },
    { -20,-10,-10,-10,-10,-10,-10,-20, -10,0,0,0,0,0,0,-10, -10,0,5,10,10,5,0,-10, -10,5,5,10,10,5,5,-10,
      -10,0,10,10,10,10,0,-10, -10,10,10,10,10,10,10,-10, -10,5,0,0,0,0,5,-10, -20,-10,-10,-10,-10,-10,-10,-20 },
    { 0,0,0,0,0,0,0,0, 5,10,10,10,10,10,10,5, -5,0,0,0,0,0,0,-5, -5,0,0,0,0,0,0,-5,
      -5,0,0,0,0,0,0,-5, -5,0,0,0,0,0,0,-5, -5,0,0,0,0,0,0,-5, 0,0,0,5,5,0,0,0 },
    { -20,-10,-10,-5,-5,-10,-10,-20, -10,0,0,0,0,0,0,-10, -10,0,5,5,5,5,0,-10, -5,0,5,5,5,5,0,-5,
      0,0,5,5,5,5,0,-5, -10,5,5,5,5,5,0,-10, -10,0,5,0,0,0,0,-10, -20,-10,-10,-5,-5,-10,-10,-20 },
    { -30,-40,-40,-50,-50,-40,-40,-30, -30,-40,-40,-50,-50,-40,-40,-30, -30,-40,-40,-50,-50,-40,-40,-30, -30,-40,-40,-50,-50,-40,-40,-30,
      -20,-30,-30,-40,-40,-30,-30,-20, -10,-20,-20,-20,-20,-20,-20,-10, 20,20,0,0,0,0,20,20, 20,30,10,0,0,10,30,20 },
};
static const int8_t KING_END[64] = {
    -50,-40,-30,-20,-20,-30,-40,-50, -30,-20,-10,0,0,-10,-20,-30, -30,-10,20,30,30,20,-10,-30, -30,-10,30,40,40,30,-10,-30,
    -30,-10,30,40,40,30,-10,-30, -30,-10,20,30,30,20,-10,-30, -30,-30,0,0,0,0,-30,-30, -50,-30,-30,-30,-30,-30,-30,-50 };

static int eval(const cpos *p) {          /* from the side to move's view */
    int sc = 0, queens = 0;
    for (int i = 0; i < 64; i++) if (p->b[i] == Q_ || p->b[i] == -Q_) queens++;
    for (int i = 0; i < 64; i++) {
        int pc = p->b[i];
        if (!pc) continue;
        int t = pc < 0 ? -pc : pc, idx = pc > 0 ? (7 - (i >> 3)) * 8 + (i & 7) : i;   /* tables are rank 8 first */
        int v = VAL[t] + (t == K_ && !queens ? KING_END[idx] : PST[t][idx]);
        sc += pc > 0 ? v : -v;
    }
    return p->side == WHITE ? sc : -sc;
}

#define INF 32000
#define MATE 30000
static uint32_t nodes, deadline;
static int timed_out, hist_len;
static cpos hist[128];                         /* positions on the current search path, for repetition draws */

static int score_move(const cpos *p, cmove m) {
    int cap = p->b[m.to] < 0 ? -p->b[m.to] : p->b[m.to], pc = p->b[m.from] < 0 ? -p->b[m.from] : p->b[m.from];
    if (pc == P_ && m.to == p->ep) cap = P_;
    return (cap ? 10000 + VAL[cap] * 10 - VAL[pc] : 0) + (m.promo ? 9000 + VAL[m.promo] : 0);
}

static void order(const cpos *p, cmove *ms, int n, cmove first) {
    int sc[256];
    for (int i = 0; i < n; i++) sc[i] = (ms[i].from == first.from && ms[i].to == first.to && ms[i].promo == first.promo) ? 1 << 20 : score_move(p, ms[i]);
    for (int i = 1; i < n; i++) {              /* insertion sort, descending */
        cmove m = ms[i]; int s = sc[i], j = i - 1;
        while (j >= 0 && sc[j] < s) { ms[j + 1] = ms[j]; sc[j + 1] = sc[j]; j--; }
        ms[j + 1] = m; sc[j + 1] = s;
    }
}

static int check_time(void) {
    if (engine_clock && (nodes & 1023) == 0 && engine_clock() >= deadline) timed_out = 1;
    return timed_out;
}

static int quiesce(const cpos *p, int alpha, int beta) {
    nodes++;
    if (check_time()) return 0;
    int st = eval(p);
    if (st >= beta) return st;
    if (st > alpha) alpha = st;
    cmove ms[256];
    int n = gen(p, ms, 1);
    order(p, ms, n, (cmove){ 0, 0, 0 });
    for (int i = 0; i < n; i++) {
        cpos q;
        cp_make(p, ms[i], &q);
        if (cp_in_check(&q, p->side)) continue;
        int v = -quiesce(&q, -beta, -alpha);
        if (timed_out) return 0;
        if (v >= beta) return v;
        if (v > alpha) alpha = v;
    }
    return alpha;
}

static int negamax(const cpos *p, int depth, int ply, int alpha, int beta) {
    nodes++;
    if (check_time()) return 0;
    if (p->half >= 100) return 0;
    for (int i = hist_len - 3; i >= 0; i -= 2)                     /* earlier positions with the same side to move */
        if (cp_same(&hist[i], p)) return 0;
    if (hist_len >= 120) return eval(p);
    int chk = cp_in_check(p, p->side);
    if (chk) depth++;                                              /* check extension */
    if (depth <= 0) return quiesce(p, alpha, beta);
    cmove ms[256];
    int n = gen(p, ms, 0), legal = 0;
    order(p, ms, n, (cmove){ 0, 0, 0 });
    for (int i = 0; i < n; i++) {
        cpos q;
        cp_make(p, ms[i], &q);
        if (cp_in_check(&q, p->side)) continue;
        legal++;
        hist[hist_len++] = q;
        int v = -negamax(&q, depth - 1, ply + 1, -beta, -alpha);
        hist_len--;
        if (timed_out) return 0;
        if (v >= beta) return v;
        if (v > alpha) alpha = v;
    }
    if (!legal) return chk ? -MATE + ply : 0;
    return alpha;
}

cmove cp_search(const cpos *p, int depth, int ms_limit, int noise, int *score) {
    cmove root[256], best = { 0, 0, 0 };
    int n = cp_moves(p, root), best_sc = -INF;
    static uint32_t seed = 12345;
    nodes = 0; timed_out = 0;
    deadline = engine_clock ? engine_clock() + ms_limit : 0;
    if (!n) { if (score) *score = 0; return best; }
    best = root[0];
    hist[0] = *p; hist_len = 1;
    for (int d = 1; d <= depth; d++) {
        cmove it_best = root[0];
        int it_sc = -INF, alpha = -INF;
        order(p, root, n, best);
        for (int i = 0; i < n; i++) {
            cpos q;
            cp_make(p, root[i], &q);
            hist[hist_len++] = q;
            int v = -negamax(&q, d - 1, 1, -INF, -alpha);
            hist_len--;
            if (timed_out) break;
            if (noise && d == depth) { seed = seed * 1103515245u + 12345u; v += (int)((seed >> 16) % (noise + 1)); }
            if (v > it_sc) { it_sc = v; it_best = root[i]; }
            if (v > alpha) alpha = v;
        }
        if (timed_out && d > 1) break;                              /* keep the last complete iteration */
        best = it_best; best_sc = it_sc;
        if (timed_out || best_sc > MATE - 100) break;
    }
    if (score) *score = best_sc;
    return best;
}
