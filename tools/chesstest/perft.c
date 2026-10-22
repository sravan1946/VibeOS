/* perft.c - move generator check against the published perft counts, plus a few rules spot checks */
#include <stdio.h>
#include <string.h>
#include "../../user/bin/chess/engine.h"

static int fails;
static void check(const char *what, uint64_t got, uint64_t want) {
    printf("%-34s %10llu %s\n", what, (unsigned long long)got, got == want ? "ok" : "FAIL");
    if (got != want) fails++;
}

/* minimal FEN reader (board, side, castling, ep) */
static void fen(cpos *p, const char *s) {
    memset(p, 0, sizeof *p);
    int sq = 56;
    for (; *s && *s != ' '; s++) {
        if (*s == '/') sq -= 16;
        else if (*s >= '1' && *s <= '8') sq += *s - '0';
        else { int v = (int)(strchr("pnbrqk", *s | 32) - "pnbrqk") + 1; p->b[sq++] = (*s >= 'a') ? -v : v; }
    }
    p->side = s[1] == 'b'; s += 3;
    for (; *s && *s != ' '; s++) p->castle |= *s == 'K' ? 1 : *s == 'Q' ? 2 : *s == 'k' ? 4 : *s == 'q' ? 8 : 0;
    p->ep = -1;
    if (s[1] != '-') p->ep = (s[1] - 'a') + (s[2] - '1') * 8;
    p->full = 1;
}

int main(void) {
    cpos p;
    char name[64];
    static const uint64_t start[] = { 20, 400, 8902, 197281 };
    cp_start(&p);
    for (int d = 1; d <= 4; d++) { snprintf(name, sizeof name, "start position depth %d", d); check(name, cp_perft(&p, d), start[d - 1]); }
    /* kiwipete: castling, en passant, promotions and pins all at once */
    fen(&p, "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
    static const uint64_t kiwi[] = { 48, 2039, 97862 };
    for (int d = 1; d <= 3; d++) { snprintf(name, sizeof name, "kiwipete depth %d", d); check(name, cp_perft(&p, d), kiwi[d - 1]); }
    fen(&p, "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1");          /* en passant discovered checks */
    static const uint64_t pos3[] = { 14, 191, 2812, 43238 };
    for (int d = 1; d <= 4; d++) { snprintf(name, sizeof name, "position 3 depth %d", d); check(name, cp_perft(&p, d), pos3[d - 1]); }
    fen(&p, "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1");   /* promotions */
    check("position 4 depth 3", cp_perft(&p, 3), 9467);
    /* status, SAN and search */
    char san[16];
    fen(&p, "7k/5Q2/6K1/8/8/8/8/8 b - - 0 1");
    check("stalemate detected", cp_status(&p), ST_STALEMATE);
    fen(&p, "R5k1/5ppp/8/8/8/8/8/6K1 b - - 0 1");
    check("back-rank mate detected", cp_status(&p), ST_MATE);
    fen(&p, "8/8/4k3/8/8/4K3/8/8 w - - 0 1");
    check("K v K insufficient", cp_status(&p), ST_INSUFFICIENT);
    fen(&p, "6k1/5ppp/8/8/8/8/8/R5K1 w - - 0 1");
    cmove m = cp_search(&p, 3, 0, 0, NULL);
    cp_san(&p, m, san);
    check("search finds Ra8#", !strcmp(san, "Ra8#"), 1);
    fen(&p, "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
    cmove ms[256];
    int n = cp_moves(&p, ms), oo = 0, ooo = 0;
    for (int i = 0; i < n; i++) { cp_san(&p, ms[i], san); oo += !strcmp(san, "O-O"); ooo += !strcmp(san, "O-O-O"); }
    check("both castles offered", oo + ooo, 2);
    puts(fails ? "chesstest: FAILED" : "chesstest: all passed");
    return fails != 0;
}
