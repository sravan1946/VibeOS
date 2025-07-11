#include "shell.h"
#include "console.h"
#include "filesystem.h"
#include "editor.h"
#include "keyboard.h"

// Minimal kernel string compare
static int kstrcmp(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a - *b;
}
static int kstrncmp(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i] || !a[i] || !b[i]) return a[i] - b[i];
    }
    return 0;
}

extern volatile unsigned int timer_irq_count;
extern volatile unsigned int key_irq_count;

void handle_command(const char* cmd) {
    // Trim leading whitespace
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    if (cmd[0] == 0) {
        print_prompt();
        return;
    }
    if (!kstrcmp(cmd, "help")) {
        print("Available commands:\n");
        print("  help   - Show this help message\n");
        print("  clear  - Clear the screen\n");
        print("  count  - Show interrupt counts\n");
        print("  echo <text> - Print text\n");
        print("  ls     - List files\n");
        print("  cat <file> - Show file contents\n");
        print("  echo <text> > <file> - Write file\n");
        print("  edit <file> - Edit a file\n");
        print_prompt();
        return;
    } else if (!kstrcmp(cmd, "clear")) {
        clear_screen();
        print("VibeOS Shell\n\n");
        print_prompt();
        return;
    } else if (!kstrcmp(cmd, "count")) {
        print("Timer IRQs: ");
        print_hex(timer_irq_count);
        print("\nKeyboard IRQs: ");
        print_hex(key_irq_count);
        print("\n");
    } else if (!kstrcmp(cmd, "ls")) {
        list_files();
    } else if (!kstrncmp(cmd, "cat ", 4)) {
        const char* fname = cmd + 4;
        int len = 0;
        const char* data = read_file(fname, &len);
        if (!data) {
            print("File not found.\n");
        } else {
            for (int i = 0; i < len; i++) print_char(data[i]);
            print("\n");
        }
    } else if (!kstrncmp(cmd, "echo ", 5)) {
        const char* gt = 0;
        for (int i = 5; cmd[i]; i++) {
            if (cmd[i] == '>' && cmd[i+1] == ' ') { gt = cmd + i; break; }
        }
        if (gt) {
            int text_len = gt - (cmd + 5);
            char text[MAX_FILESIZE];
            for (int i = 0; i < text_len && i < MAX_FILESIZE; i++) text[i] = cmd[5 + i];
            const char* fname = gt + 2;
            if (write_file(fname, text, text_len)) {
                print("Wrote file: "); print(fname); print("\n");
            } else {
                print("File write error.\n");
            }
        } else {
            print(cmd + 5);
            print("\n");
        }
    } else if (!kstrncmp(cmd, "edit ", 5)) {
        editor(cmd + 5);
        return;
    } else {
        print("Unknown command: ");
        print(cmd);
        print("\n");
    }
    print_prompt();
}

// Add a simple shell main loop for polling input (fallback if interrupts fail)
void shell_main(void) {
    print("VibeOS Shell\n\n");
    print_prompt();
    char buf[INPUT_BUFFER_SIZE];
    unsigned int len = 0;
    while (1) {
        // Poll for keyboard input (only read if key is available)
        unsigned char status = inb(0x64);
        if (status & 0x01) { // Output buffer full
            unsigned char scancode = inb(0x60);
            char c = 0;
            if (scancode < 128) {
                c = shift_pressed ? scancode_map_shift[scancode] : scancode_map[scancode];
            }
            if (c == '\n' || c == '\r') {
                print_char('\n');
                buf[len] = 0;
                handle_command(buf);
                len = 0;
            } else if (c == '\b') {
                if (len > 0) {
                    len--;
                    print_char('\b');
                }
            } else if (is_printable(c) && len < INPUT_BUFFER_SIZE - 1) {
                buf[len++] = c;
                print_char(c);
            }
        }
    }
} 