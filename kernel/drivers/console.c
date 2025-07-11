#include "console.h"
#include "../fs/filesystem.h"

unsigned char cursor_row = 0;
unsigned char cursor_col = 0;

void scroll_screen(void) {
    for (int row = 1; row < VGA_HEIGHT; row++) {
        for (int col = 0; col < VGA_WIDTH; col++) {
            VGA_BUFFER[(row-1)*VGA_WIDTH + col] = VGA_BUFFER[row*VGA_WIDTH + col];
        }
    }
    // Clear last line
    for (int col = 0; col < VGA_WIDTH; col++) {
        VGA_BUFFER[(VGA_HEIGHT-1)*VGA_WIDTH + col] = (WHITE_ON_BLACK << 8) | ' ';
    }
}

void print_char(char c) {
    if (c == '\n') {
        cursor_row++;
        cursor_col = 0;
    } else if (c == '\b') {
        if (cursor_col > 0) {
            cursor_col--;
            VGA_BUFFER[cursor_row * VGA_WIDTH + cursor_col] = (WHITE_ON_BLACK << 8) | ' ';
        } else if (cursor_row > 0) {
            cursor_row--;
            cursor_col = VGA_WIDTH - 1;
            VGA_BUFFER[cursor_row * VGA_WIDTH + cursor_col] = (WHITE_ON_BLACK << 8) | ' ';
        }
    } else {
        VGA_BUFFER[cursor_row * VGA_WIDTH + cursor_col] = (WHITE_ON_BLACK << 8) | c;
        cursor_col++;
        if (cursor_col >= VGA_WIDTH) {
            cursor_col = 0;
            cursor_row++;
        }
    }
    if (cursor_row >= VGA_HEIGHT) {
        scroll_screen();
        cursor_row = VGA_HEIGHT - 1;
    }
    update_cursor();
}

void print(const char* str) {
    while (*str) print_char(*str++);
}

void clear_screen(void) {
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        VGA_BUFFER[i] = (WHITE_ON_BLACK << 8) | ' ';
    }
    cursor_row = 0;
    cursor_col = 0;
    update_cursor();
}

void print_prompt(void) {
    // Print current working directory before prompt
    char buf[MAX_FILENAME*MAX_FILES];
    int pos = MAX_FILENAME*MAX_FILES-1;
    buf[pos] = 0;
    fs_node* n = current_directory;
    while (n && n != fs_root) {
        int len = 0; while (n->name[len]) len++;
        pos -= len;
        for (int i = 0; i < len; i++) buf[pos+i] = n->name[i];
        pos--;
        buf[pos+1] = '/';
        n = n->parent;
    }
    if (pos == MAX_FILENAME*MAX_FILES-1) {
        print("/");
    } else {
        print(&buf[pos+1]);
    }
    print(" > ");
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
    unsigned short pos = cursor_row * VGA_WIDTH + cursor_col;
    outb(0x3D4, 0x0F);
    outb(0x3D5, (unsigned char)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (unsigned char)((pos >> 8) & 0xFF));
} 