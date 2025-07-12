#include "graphics.h"
#include "../include/stdint.h"
#include "font8x8.h"

#define CHAR_WIDTH 8
#define CHAR_HEIGHT 8
#define CONSOLE_COLS (screen_width / (CHAR_WIDTH * 2))
#define CONSOLE_ROWS (screen_height / (CHAR_HEIGHT * 2))

static int console_cursor_x = 0;
static int console_cursor_y = 0;
static uint8_t console_fg = 15; // white
static uint8_t console_bg = 0;  // black

VBEModeInfoBlock vbe_mode_info;
uint8_t* framebuffer = 0;
uint16_t screen_width = 0, screen_height = 0, screen_pitch = 0;
uint8_t screen_bpp = 0;

void graphics_init(void) {
    // Copy VBE mode info from physical address 0x90000
    VBEModeInfoBlock* src = (VBEModeInfoBlock*)0x90000;
    for (unsigned i = 0; i < sizeof(VBEModeInfoBlock); i++) {
        ((uint8_t*)&vbe_mode_info)[i] = ((uint8_t*)src)[i];
    }
    framebuffer = (uint8_t*)(uintptr_t)vbe_mode_info.framebuffer;
    screen_width = vbe_mode_info.width;
    screen_height = vbe_mode_info.height;
    screen_pitch = vbe_mode_info.pitch;
    screen_bpp = vbe_mode_info.bpp;
}

void draw_pixel(int x, int y, uint8_t color) {
    if (!framebuffer) return;
    if (x < 0 || y < 0 || x >= screen_width || y >= screen_height) return;
    uint32_t offset = y * screen_pitch + x;
    framebuffer[offset] = color;
}

void graphics_clear_screen(uint8_t color) {
    if (!framebuffer) return;
    for (int y = 0; y < screen_height; y++) {
        for (int x = 0; x < screen_width; x++) {
            draw_pixel(x, y, color);
        }
    }
}

void graphics_console_set_color(uint8_t fg, uint8_t bg) {
    console_fg = fg;
    console_bg = bg;
}

void draw_char(int cx, int cy, char c, uint8_t fg, uint8_t bg) {
    if (c < 0x20 || c > 0x7F) c = '?';
    const uint8_t* glyph = font8x8[c - 0x20];
    int px = cx * CHAR_WIDTH * 2; // 2x scale
    int py = cy * CHAR_HEIGHT * 2; // 2x scale
    for (int y = 0; y < CHAR_HEIGHT; y++) {
        uint8_t row = glyph[y];
        for (int x = 0; x < CHAR_WIDTH; x++) {
            // Fix bit order: use (row & (1 << x))
            uint8_t color = (row & (1 << x)) ? fg : bg;
            // Draw 2x2 block for each font pixel
            draw_pixel(px + x*2,     py + y*2,     color);
            draw_pixel(px + x*2 + 1, py + y*2,     color);
            draw_pixel(px + x*2,     py + y*2 + 1, color);
            draw_pixel(px + x*2 + 1, py + y*2 + 1, color);
        }
    }
}

void graphics_console_clear() {
    graphics_clear_screen(0); // Black
    console_cursor_x = 0;
    console_cursor_y = 0;
}

static void graphics_console_scroll() {
    // Scroll up by one row (now 16 pixels due to 2x scaling)
    int row_pixels = CHAR_HEIGHT * 2;
    for (int y = row_pixels; y < screen_height; y++) {
        for (int x = 0; x < screen_width; x++) {
            uint32_t src = y * screen_pitch + x;
            uint32_t dst = (y - row_pixels) * screen_pitch + x;
            framebuffer[dst] = framebuffer[src];
        }
    }
    // Clear last row
    for (int y = screen_height - row_pixels; y < screen_height; y++) {
        for (int x = 0; x < screen_width; x++) {
            uint32_t dst = y * screen_pitch + x;
            framebuffer[dst] = 0;
        }
    }
    console_cursor_y = CONSOLE_ROWS - 1;
}

void graphics_console_putc(char c) {
    if (c == '\n') {
        console_cursor_x = 0;
        console_cursor_y++;
        if (console_cursor_y >= CONSOLE_ROWS) {
            graphics_console_scroll();
        }
        return;
    } else if (c == '\r') {
        console_cursor_x = 0;
        return;
    } else if (c == '\b') {
        if (console_cursor_x > 0) {
            console_cursor_x--;
            draw_char(console_cursor_x, console_cursor_y, ' ', console_fg, console_bg);
        }
        return;
    }
    draw_char(console_cursor_x, console_cursor_y, c, console_fg, console_bg);
    console_cursor_x++;
    if (console_cursor_x >= CONSOLE_COLS) {
        console_cursor_x = 0;
        console_cursor_y++;
        if (console_cursor_y >= CONSOLE_ROWS) {
            graphics_console_scroll();
        }
    }
}

void graphics_console_print(const char* s) {
    while (*s) graphics_console_putc(*s++);
} 