#ifndef KEYBOARD_H
#define KEYBOARD_H

#define INPUT_BUFFER_SIZE 128
extern char input_buffer[INPUT_BUFFER_SIZE];
extern unsigned int input_len;
extern volatile int shift_pressed;
extern volatile int extended_scancode;
extern char last_command[INPUT_BUFFER_SIZE];

extern const char scancode_map[128];
extern const char scancode_map_shift[128];
int is_printable(char c);
unsigned char inb(unsigned short port);

void keyboard_handler(void);
void timer_handler(void);

#endif // KEYBOARD_H 