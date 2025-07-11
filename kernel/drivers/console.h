#ifndef CONSOLE_H
#define CONSOLE_H

#define VGA_ADDRESS 0xB8000
#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define WHITE_ON_BLACK 0x0F
#define VGA_BUFFER ((volatile unsigned short*)VGA_ADDRESS)

// Removed: extern volatile unsigned short* vga_buffer;
extern unsigned char cursor_row;
extern unsigned char cursor_col;

void print_char(char c);
void print(const char* str);
void clear_screen(void);
void print_prompt(void);
void print_hex(unsigned int val);
void update_cursor(void);
void scroll_screen(void);

// outb is needed for update_cursor and other hardware I/O
static inline void outb(unsigned short port, unsigned char val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

#endif // CONSOLE_H 