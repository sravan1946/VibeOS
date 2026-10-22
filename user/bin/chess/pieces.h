/* pieces.h - rendered chess piece sprites (straight-alpha ARGB, size x size) */
#pragma once
#include <gfx.h>

/* (re)build sprite `set` (0 = board, 1 = small) at size x size pixels; a no-op if already that size */
void pieces_build(int set, int size);
/* piece = +/-1..6 as in engine.h, white positive */
const uint32_t *piece_sprite(int set, int piece);
