#include "console.h"
#include "../fs/filesystem.h"
#include "graphics.h"

unsigned char cursor_row = 0;
unsigned char cursor_col = 0;

void scroll_screen(void) {
    // No-op in graphics mode
}

void print_char(char c) {
    graphics_console_putc(c);
}

void print(const char* str) {
    graphics_console_print(str);
}

void clear_screen(void) {
    graphics_console_clear();
}

void print_hex(unsigned int val) {
    char hex[11] = "0x00000000";
    for (int i = 9; i >= 2; --i) {
        int v = val & 0xF;
        hex[i] = (v < 10) ? ('0' + v) : ('A' + v - 10);
        val >>= 4;
    }
    print(hex);
}

void update_cursor(void) {
    // No-op in graphics mode
}

void print_prompt(void) {
    // Print current working directory before prompt
    char path[MAX_FILENAME*MAX_FILES];
    get_current_path(path, sizeof(path));
    graphics_console_set_color(rgb16(0,255,0), rgb16(0,0,0)); // green path
    print(path);
    graphics_console_set_color(rgb16(255,255,255), rgb16(0,0,0)); // white prompt
    print(" > ");
    graphics_console_set_color(rgb16(255,255,255), rgb16(0,0,0)); // restore white on black
} 