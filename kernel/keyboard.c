#include "keyboard.h"
#include "console.h"
#include "shell.h"

char input_buffer[INPUT_BUFFER_SIZE];
unsigned int input_len = 0;
char last_command[INPUT_BUFFER_SIZE] = {0};
volatile int shift_pressed = 0;
volatile int extended_scancode = 0;

extern volatile unsigned int timer_irq_count;
extern volatile unsigned int key_irq_count;

const char scancode_map[128] = {
    0,   27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,   'a','s','d','f','g','h','j','k','l',';','\'','`', 0,
    '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0,  ' ', 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0
};

const char scancode_map_shift[128] = {
    0,   27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,   'A','S','D','F','G','H','J','K','L',':','\"','~', 0,
    '|','Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0,  ' ', 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0
};

int is_printable(char c) {
    return (c >= 32 && c <= 126);
}

unsigned char inb(unsigned short port) {
    unsigned char ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void timer_handler(void) {
    static int first_timer = 1;
    if (first_timer) {
        print("[timer IRQ working]\n");
        first_timer = 0;
    }
    timer_irq_count++;
    outb(0x20, 0x20);
}

// Add this helper to print input buffer as hex
static void print_input_hex(const char* buf) {
    print("[input hex: ");
    for (int i = 0; buf[i] && i < INPUT_BUFFER_SIZE; i++) {
        char hex[4] = {0};
        unsigned char c = buf[i];
        hex[0] = "0123456789ABCDEF"[(c >> 4) & 0xF];
        hex[1] = "0123456789ABCDEF"[c & 0xF];
        hex[2] = ' ';
        hex[3] = 0;
        print(hex);
    }
    print("]\n");
}

// Helper to trim trailing whitespace
static void trim_input(char* buf) {
    int len = 0;
    while (buf[len]) len++;
    while (len > 0 && (buf[len-1] == ' ' || buf[len-1] == '\r' || buf[len-1] == '\n' || buf[len-1] == '\t')) {
        buf[len-1] = 0;
        len--;
    }
}

void keyboard_handler(void) {
    static int first_key = 1;
    if (first_key) {
        print("[keyboard IRQ working]\n");
        first_key = 0;
    }
    key_irq_count++;
    unsigned char scancode = inb(0x60);
    // Handle extended scancode prefix
    if (scancode == 0xE0) {
        extended_scancode = 1;
        outb(0x20, 0x20);
        return;
    }
    // Handle Shift key press/release
    if (!extended_scancode && (scancode == 0x2A || scancode == 0x36)) { // Left or Right Shift pressed
        shift_pressed = 1;
        outb(0x20, 0x20);
        return;
    } else if (!extended_scancode && (scancode == 0xAA || scancode == 0xB6)) { // Left or Right Shift released
        shift_pressed = 0;
        outb(0x20, 0x20);
        return;
    }
    // Handle extended keys
    if (extended_scancode) {
        // Up arrow: 0x48 (press), 0xC8 (release)
        if (scancode == 0x48) { // Up arrow pressed
            // Recall last command
            // Erase current input from screen
            while (input_len > 0) {
                print_char('\b');
                input_len--;
            }
            // Copy last_command to input_buffer
            for (unsigned int i = 0; i < INPUT_BUFFER_SIZE; i++) {
                input_buffer[i] = last_command[i];
                if (last_command[i] == 0) break;
            }
            input_len = 0;
            while (input_buffer[input_len]) {
                print_char(input_buffer[input_len]);
                input_len++;
            }
        }
        extended_scancode = 0;
        outb(0x20, 0x20);
        return;
    }
    if (scancode & 0x80) {
        // Key release, ignore (except Shift handled above)
        outb(0x20, 0x20); // Always send EOI
        return;
    }
    // Use shifted or unshifted map
    char c = shift_pressed ? scancode_map_shift[scancode] : scancode_map[scancode];
    if (c == '\b') {
        if (input_len > 0) {
            input_len--;
            print_char('\b');
        }
    } else if (c == '\n' || c == '\r') {
        print_char('\n');
        input_buffer[input_len] = 0;
        trim_input(input_buffer);
        // Store last command
        for (unsigned int i = 0; i < INPUT_BUFFER_SIZE; i++) {
            last_command[i] = input_buffer[i];
            if (input_buffer[i] == 0) break;
        }
        handle_command(input_buffer);
        input_len = 0;
        // Do NOT print prompt here; handle_command prints it as needed
    } else if (is_printable(c) && input_len < INPUT_BUFFER_SIZE - 1) {
        input_buffer[input_len++] = c;
        print_char(c);
    }
    outb(0x20, 0x20);
} 