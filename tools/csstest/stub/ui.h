/* host-test stand-in for user/lib/ui.h: just what browser.h's CSS part needs */
#pragma once
#include <stdint.h>
typedef uint32_t color_t;
#define RGB(r, g, b) (0xFF000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
struct ui; struct canvas; struct gui_event;
