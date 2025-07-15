#include "console.h"
#include "graphics.h"
#include "keyboard.h"
#include "font8x8.h"

// Forward declaration for outb
void outb(unsigned short port, unsigned char val);

#define CHAR_WIDTH 8
#define CHAR_HEIGHT 8
#define CONSOLE_COLS (screen_width / (CHAR_WIDTH * 2))
#define CONSOLE_ROWS (screen_height / (CHAR_HEIGHT * 2))

static int console_cursor_x = 0;
static int console_cursor_y = 0;
static uint16_t console_fg = 0xFFFF; // white (16bpp)
static uint16_t console_bg = 0x0000; // black (16bpp)

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

// Helper: convert 8-bit RGB to 16-bit 5:6:5
uint16_t rgb16(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

void draw_pixel(int x, int y, uint16_t color) {
    if (!framebuffer) return;
    if (x < 0 || y < 0 || x >= screen_width || y >= screen_height) return;
    if (screen_bpp == 16) {
        uint32_t offset = y * screen_pitch + x * 2;
        framebuffer[offset] = color & 0xFF;
        framebuffer[offset + 1] = (color >> 8) & 0xFF;
    } else if (screen_bpp == 8) {
        uint32_t offset = y * screen_pitch + x;
        framebuffer[offset] = (uint8_t)color;
    }
}

void graphics_clear_screen(uint16_t color) {
    if (!framebuffer) return;
    for (int y = 0; y < screen_height; y++) {
        for (int x = 0; x < screen_width; x++) {
            draw_pixel(x, y, color);
        }
    }
}

void graphics_console_set_color(uint16_t fg, uint16_t bg) {
    console_fg = fg;
    console_bg = bg;
}

void draw_char(int cx, int cy, char c, uint16_t fg, uint16_t bg) {
    if (c < 0x20 || c > 0x7F) c = '?';
    const uint8_t* glyph = font8x8[c - 0x20];
    int px = cx * CHAR_WIDTH * 2; // 2x scale
    int py = cy * CHAR_HEIGHT * 2; // 2x scale
    for (int y = 0; y < CHAR_HEIGHT; y++) {
        uint8_t row = glyph[y];
        for (int x = 0; x < CHAR_WIDTH; x++) {
            uint16_t color = (row & (1 << x)) ? fg : bg;
            draw_pixel(px + x*2,     py + y*2,     color);
            draw_pixel(px + x*2 + 1, py + y*2,     color);
            draw_pixel(px + x*2,     py + y*2 + 1, color);
            draw_pixel(px + x*2 + 1, py + y*2 + 1, color);
        }
    }
}

void graphics_console_clear() {
    graphics_clear_screen(rgb16(0,0,0)); // Black
    console_cursor_x = 0;
    console_cursor_y = 0;
}

static void graphics_console_scroll() {
    // Scroll up by one row (now 16 pixels due to 2x scaling)
    int row_pixels = CHAR_HEIGHT * 2;
    if (screen_bpp == 16) {
        for (int y = row_pixels; y < screen_height; y++) {
            for (int x = 0; x < screen_width; x++) {
                uint32_t src = y * screen_pitch + x * 2;
                uint32_t dst = (y - row_pixels) * screen_pitch + x * 2;
                framebuffer[dst] = framebuffer[src];
                framebuffer[dst + 1] = framebuffer[src + 1];
            }
        }
        // Clear last row
        for (int y = screen_height - row_pixels; y < screen_height; y++) {
            for (int x = 0; x < screen_width; x++) {
                uint32_t dst = y * screen_pitch + x * 2;
                framebuffer[dst] = 0;
                framebuffer[dst + 1] = 0;
            }
        }
    } else if (screen_bpp == 8) {
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

static int kstrlen(const char* s) { int i = 0; while (s[i]) i++; return i; }

void show_splash_screen(void) {
    // 1. Diagonal purple-to-orange gradient background (16bpp)
    uint8_t purple_r = 128, purple_g = 0, purple_b = 192;
    uint8_t orange_r = 255, orange_g = 128, orange_b = 0;
    for (int y = 0; y < screen_height; y++) {
        for (int x = 0; x < screen_width; x++) {
            float t = ((float)x / (float)(screen_width - 1) + (float)y / (float)(screen_height - 1)) / 2.0f;
            uint8_t r = (uint8_t)((1.0f - t) * purple_r + t * orange_r);
            uint8_t g = (uint8_t)((1.0f - t) * purple_g + t * orange_g);
            uint8_t b = (uint8_t)((1.0f - t) * purple_b + t * orange_b);
            draw_pixel(x, y, rgb16(r, g, b));
        }
    }
    // 2. Draw a single clean border rectangle around the text area
    int rect_w = screen_width / 2;
    int rect_h = screen_height / 4;
    int rect_x = (screen_width - rect_w) / 2;
    int rect_y = (screen_height - rect_h) / 2 - 32;
    uint16_t border = rgb16(255,255,255);
    for (int i = 0; i < rect_w; i++) {
        draw_pixel(rect_x + i, rect_y, border);
        draw_pixel(rect_x + i, rect_y + rect_h - 1, border);
    }
    for (int i = 0; i < rect_h; i++) {
        draw_pixel(rect_x, rect_y + i, border);
        draw_pixel(rect_x + rect_w - 1, rect_y + i, border);
    }
    // 3. Centered VibeOS text (no shadow)
    const char* name = "VibeOS";
    int name_len = kstrlen(name);
    int name_cx = (screen_width / (CHAR_WIDTH * 2) - name_len) / 2;
    int name_cy = ((rect_y + rect_h/2) / (CHAR_HEIGHT * 2)) - 1;
    for (int i = 0; i < name_len; i++) {
        int px = (name_cx + i) * CHAR_WIDTH * 2;
        int py = name_cy * CHAR_HEIGHT * 2;
        float t = ((float)px / (float)(screen_width - 1) + (float)py / (float)(screen_height - 1)) / 2.0f;
        uint8_t r = (uint8_t)((1.0f - t) * purple_r + t * orange_r);
        uint8_t g = (uint8_t)((1.0f - t) * purple_g + t * orange_g);
        uint8_t b = (uint8_t)((1.0f - t) * purple_b + t * orange_b);
        uint16_t bg = rgb16(r, g, b);
        draw_char(name_cx + i, name_cy, name[i], rgb16(255,255,255), bg);
    }
    // 4. Tagline (no shadow)
    const char* tagline = "A Vibe-Coded OS";
    int tag_len = kstrlen(tagline);
    int tag_cx = (screen_width / (CHAR_WIDTH * 2) - tag_len) / 2;
    int tag_cy = name_cy + 2;
    for (int i = 0; i < tag_len; i++) {
        int px = (tag_cx + i) * CHAR_WIDTH * 2;
        int py = tag_cy * CHAR_HEIGHT * 2;
        float t = ((float)px / (float)(screen_width - 1) + (float)py / (float)(screen_height - 1)) / 2.0f;
        uint8_t r = (uint8_t)((1.0f - t) * purple_r + t * orange_r);
        uint8_t g = (uint8_t)((1.0f - t) * purple_g + t * orange_g);
        uint8_t b = (uint8_t)((1.0f - t) * purple_b + t * orange_b);
        uint16_t bg = rgb16(r, g, b);
        draw_char(tag_cx + i, tag_cy, tagline[i], rgb16(255,200,128), bg);
    }
    // 5. Blinking prompt below the rectangle (no shadow)
    const char* prompt = "Press any key to continue...";
    int prompt_len = kstrlen(prompt);
    int prompt_cx = (screen_width / (CHAR_WIDTH * 2) - prompt_len) / 2;
    int prompt_cy = (rect_y + rect_h) / (CHAR_HEIGHT * 2) + 2;
    // Flush any pending keypresses
    while (1) {
        unsigned char status = inb(0x64);
        if (status & 0x01) { (void)inb(0x60); }
        else break;
    }
    int blink = 0;
    while (1) {
        for (int i = 0; i < prompt_len; i++) {
            int px = (prompt_cx + i) * CHAR_WIDTH * 2;
            int py = prompt_cy * CHAR_HEIGHT * 2;
            float t = ((float)px / (float)(screen_width - 1) + (float)py / (float)(screen_height - 1)) / 2.0f;
            uint8_t r = (uint8_t)((1.0f - t) * purple_r + t * orange_r);
            uint8_t g = (uint8_t)((1.0f - t) * purple_g + t * orange_g);
            uint8_t b = (uint8_t)((1.0f - t) * purple_b + t * orange_b);
            uint16_t bg = rgb16(r, g, b);
            draw_char(prompt_cx + i, prompt_cy, prompt[i], blink ? rgb16(128,128,128) : rgb16(255,255,255), bg);
        }
        unsigned char status = inb(0x64);
        if (status & 0x01) { (void)inb(0x60); break; }
        for (volatile int d = 0; d < 1000000; d++);
        blink = !blink;
    }
    outb(0x20, 0x20); // Send EOI for keyboard IRQ
} 