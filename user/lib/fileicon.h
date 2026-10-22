/* fileicon.h - scalable file-type icons (shared by files and desktop). Link order: libgui.a before libc.a. */
#pragma once
#include "gfx.h"
#include "fsutil.h"
/* kind: FK_*; s = size of the square box at x,y (20 for lists, 48-64 for icon grids); name supplies the extension badge */
void fileicon(struct canvas *c, int kind, int x, int y, int s, const char *name);
