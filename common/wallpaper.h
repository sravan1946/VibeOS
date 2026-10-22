/* wallpaper.h - procedural desktop backgrounds */
#pragma once
#include "gfx.h"

int wallpaper_count(void);
const char *wallpaper_name(int theme);
void wallpaper_render(struct canvas *c, int theme);   /* fills the whole canvas */
void wallpaper_watermark(struct canvas *c);           /* the "VibeOS" mark, bottom right */
