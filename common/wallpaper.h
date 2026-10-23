/* wallpaper.h - procedural desktop backgrounds */
#pragma once
#include "gfx.h"

int wallpaper_count(void);
const char *wallpaper_name(int theme);                /* 0 = "Dunes", the default */
void wallpaper_render(struct canvas *c, int theme);   /* fills the whole canvas */
