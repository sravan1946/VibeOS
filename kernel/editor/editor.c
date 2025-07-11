#include "editor.h"
#include "../drivers/console.h"
#include "../fs/filesystem.h"
#include "../drivers/keyboard.h"

static int kstrlen(const char* s) {
    int i = 0;
    while (s[i]) i++;
    return i;
}

void editor(const char* filename) {
    char lines[EDITOR_MAX_LINES][EDITOR_MAX_LINE_LEN+1] = {{0}};
    int num_lines = 1;
    int cur_line = 0, cur_col = 0;
    int running = 1;
    int saved = 0;
    // Load file if exists
    int file_len = 0;
    const char* file_data = read_file(filename, &file_len);
    if (file_data) {
        int l = 0, c = 0;
        for (int i = 0; i < file_len && l < EDITOR_MAX_LINES; i++) {
            if (file_data[i] == '\n' || c >= EDITOR_MAX_LINE_LEN) {
                lines[l][c] = 0;
                l++; c = 0;
                if (file_data[i] != '\n' && l < EDITOR_MAX_LINES) lines[l][c++] = file_data[i];
            } else {
                lines[l][c++] = file_data[i];
            }
        }
        num_lines = l+1;
    }
    clear_screen();
    print("-- Nano Editor --  ^S Save  ^Q Quit\n\n");
    for (int i = 0; i < num_lines; i++) {
        print(lines[i]);
        print("\n");
    }
    cursor_row = 2 + cur_line;
    cursor_col = cur_col;
    update_cursor();
    int extended = 0;
    while (running) {
        // Only read scancode if a key is available
        unsigned char status = inb(0x64);
        if (!(status & 0x01)) continue;
        unsigned char scancode = inb(0x60);
        if (scancode == 0xE0) { extended = 1; continue; }
        // Modifier key handling (update global state)
        if (scancode == 0x2A || scancode == 0x36) { // Shift pressed
            shift_pressed = 1;
            continue;
        } else if (scancode == 0xAA || scancode == 0xB6) { // Shift released
            shift_pressed = 0;
            continue;
        } else if (scancode == 0x1D) { // Ctrl pressed
            ctrl_pressed = 1;
            continue;
        } else if (scancode == 0x9D) { // Ctrl released
            ctrl_pressed = 0;
            continue;
        }
        // Handle arrow keys if extended
        if (extended) {
            if (scancode == 0x4B) { // Left arrow
                if (cur_col > 0) cur_col--;
                else if (cur_line > 0) {
                    cur_line--;
                    int len = kstrlen(lines[cur_line]);
                    cur_col = len > 0 ? len : 0;
                }
            } else if (scancode == 0x4D) { // Right arrow
                int len = kstrlen(lines[cur_line]);
                if (cur_col < len) cur_col++;
                else if (cur_line < num_lines-1) {
                    cur_line++;
                    cur_col = 0;
                }
            } else if (scancode == 0x48) { // Up arrow
                if (cur_line > 0) {
                    cur_line--;
                    int len = kstrlen(lines[cur_line]);
                    if (cur_col > len) cur_col = len;
                }
            } else if (scancode == 0x50) { // Down arrow
                if (cur_line < num_lines-1) {
                    cur_line++;
                    int len = kstrlen(lines[cur_line]);
                    if (cur_col > len) cur_col = len;
                }
            }
            extended = 0;
            clear_screen();
            print("-- Nano Editor --  ^S Save  ^Q Quit\n\n");
            for (int i = 0; i < num_lines; i++) {
                print(lines[i]);
                print("\n");
            }
            cursor_row = 2 + cur_line;
            cursor_col = cur_col;
            update_cursor();
            continue;
        }
        char c = 0;
        if (scancode < 128) {
            c = shift_pressed ? scancode_map_shift[scancode] : scancode_map[scancode];
        }
        // Handle Ctrl+S and Ctrl+Q
        if (ctrl_pressed && (c == 's' || c == 'S')) { // Ctrl+S
            int total = 0;
            for (int l = 0; l < num_lines; l++) total += kstrlen(lines[l]) + 1;
            char buf[EDITOR_MAX_LINES * (EDITOR_MAX_LINE_LEN+1)];
            int pos = 0;
            for (int l = 0; l < num_lines; l++) {
                int len = kstrlen(lines[l]);
                for (int k = 0; k < len; k++) buf[pos++] = lines[l][k];
                buf[pos++] = '\n';
            }
            write_file(filename, buf, pos);
            saved = 1;
            running = 0;
        } else if (ctrl_pressed && (c == 'q' || c == 'Q')) { // Ctrl+Q
            running = 0;
        } else if (c == '\n') {
            if (num_lines < EDITOR_MAX_LINES) {
                for (int l = num_lines; l > cur_line+1; l--) {
                    for (int k = 0; k < EDITOR_MAX_LINE_LEN+1; k++)
                        lines[l][k] = lines[l-1][k];
                }
                for (int k = 0; k < EDITOR_MAX_LINE_LEN+1; k++) lines[cur_line+1][k] = 0;
                num_lines++;
                cur_line++;
                cur_col = 0;
            }
        } else if (c == '\b') {
            if (cur_col > 0) {
                for (int k = cur_col-1; k < EDITOR_MAX_LINE_LEN-1; k++)
                    lines[cur_line][k] = lines[cur_line][k+1];
                lines[cur_line][EDITOR_MAX_LINE_LEN-1] = 0;
                cur_col--;
            } else if (cur_line > 0) {
                int prev_len = 0;
                while (lines[cur_line-1][prev_len]) prev_len++;
                if (prev_len + kstrlen(lines[cur_line]) < EDITOR_MAX_LINE_LEN) {
                    for (int k = 0; lines[cur_line][k]; k++)
                        lines[cur_line-1][prev_len+k] = lines[cur_line][k];
                    lines[cur_line-1][prev_len+kstrlen(lines[cur_line])] = 0;
                    for (int l = cur_line; l < num_lines-1; l++)
                        for (int k = 0; k < EDITOR_MAX_LINE_LEN+1; k++)
                            lines[l][k] = lines[l+1][k];
                    num_lines--;
                    cur_line--;
                    cur_col = prev_len;
                }
            }
        } else if (is_printable(c)) {
            int len = kstrlen(lines[cur_line]);
            if (len < EDITOR_MAX_LINE_LEN-1) {
                for (int k = len; k > cur_col; k--)
                    lines[cur_line][k] = lines[cur_line][k-1];
                lines[cur_line][cur_col] = c;
                cur_col++;
            }
        }
        clear_screen();
        print("-- Nano Editor --  ^S Save  ^Q Quit\n\n");
        for (int i = 0; i < num_lines; i++) {
            print(lines[i]);
            print("\n");
        }
        cursor_row = 2 + cur_line;
        cursor_col = cur_col;
        update_cursor();
    }
    clear_screen();
    if (saved) {
        print("Wrote file: "); print(filename); print("\n");
    } else {
        print("Edit cancelled.\n");
    }
    print_prompt();
    // Wait for and discard the next keypress (to eat the leftover Ctrl+S/Q)
    while (1) {
        unsigned char status = inb(0x64);
        if (status & 0x01) { (void)inb(0x60); break; }
    }
} 