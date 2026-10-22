/* engine.h - chess rules and a small alpha-beta searcher (no libc beyond string.h, so tools/chesstest builds it on the host).
 * Squares are rank * 8 + file with a1 = 0; pieces are +/-1..6 = pawn knight bishop rook queen king, white positive. */
#pragma once
#include <stdint.h>

enum { P_ = 1, N_, B_, R_, Q_, K_ };
enum { WHITE = 0, BLACK = 1 };
enum { CASTLE_WK = 1, CASTLE_WQ = 2, CASTLE_BK = 4, CASTLE_BQ = 8 };
enum { ST_PLAY, ST_MATE, ST_STALEMATE, ST_INSUFFICIENT, ST_FIFTY };

typedef struct { int8_t b[64]; int8_t side; uint8_t castle; int8_t ep; uint8_t half; uint16_t full; } cpos;
typedef struct { uint8_t from, to, promo; } cmove;     /* promo = 0 or N_/B_/R_/Q_ */

void cp_start(cpos *p);
int  cp_moves(const cpos *p, cmove *out);               /* legal moves (max 256) */
void cp_make(const cpos *p, cmove m, cpos *out);
int  cp_in_check(const cpos *p, int side);
int  cp_status(const cpos *p);
int  cp_same(const cpos *a, const cpos *b);             /* same position for repetition purposes */
void cp_san(const cpos *p, cmove m, char *out);         /* "Nbd7", "exd5", "O-O", "e8=Q+" ... */
uint64_t cp_perft(const cpos *p, int depth);

/* iterative deepening to `depth` plies or until `ms` have passed on engine_clock (NULL = no limit);
 * `noise` adds up to that many centipawns of random jitter to each root score (weak levels) */
extern uint32_t (*engine_clock)(void);
cmove cp_search(const cpos *p, int depth, int ms, int noise, int *score);
