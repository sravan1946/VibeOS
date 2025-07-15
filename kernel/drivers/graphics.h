#ifndef GRAPHICS_H
#define GRAPHICS_H

#include "../include/stdint.h"

// VBE Mode Info Block (partial, only needed fields)
typedef struct {
    uint16_t attributes;
    uint8_t  winA, winB;
    uint16_t granularity;
    uint16_t winsize;
    uint16_t segmentA, segmentB;
    uint32_t realFctPtr;
    uint16_t pitch; // bytes per scanline
    uint16_t width, height;
    uint8_t  wChar, yChar, planes, bpp, banks;
    uint8_t  memory_model, bank_size, image_pages;
    uint8_t  reserved0;
    uint8_t  red_mask, red_position;
    uint8_t  green_mask, green_position;
    uint8_t  blue_mask, blue_position;
    uint8_t  reserved_mask, reserved_position;
    uint8_t  directcolor_attributes;
    uint32_t framebuffer; // physical address of linear framebuffer
    // ... (rest omitted)
} __attribute__((packed)) VBEModeInfoBlock;

extern VBEModeInfoBlock vbe_mode_info;
extern uint8_t* framebuffer;
extern uint16_t screen_width, screen_height;
extern uint16_t screen_pitch;
extern uint8_t screen_bpp;

void graphics_init(void);
void draw_pixel(int x, int y, uint16_t color);
void graphics_clear_screen(uint16_t color);
void draw_char(int cx, int cy, char c, uint16_t fg, uint16_t bg);
void graphics_console_putc(char c);
void graphics_console_print(const char* s);
void graphics_console_clear(void);
void graphics_console_set_color(uint16_t fg, uint16_t bg);

void show_splash_screen(void);
uint16_t rgb16(uint8_t r, uint8_t g, uint8_t b);

#endif // GRAPHICS_H 