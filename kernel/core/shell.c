/*
 * shell.c - VibeOS Shell
 *
 * This file implements the VibeOS shell, which now supports command-line flags for all built-in commands.
 *
 * Features:
 *   - All commands accept flags (e.g., --help, -h) and print a help message when --help is passed.
 *   - Command parsing is refactored to support flags and arguments for each command.
 *   - The parse_command helper extracts the command, a single flag (if present), and the remaining arguments.
 *
 * Usage examples:
 *   help --help
 *   clear --help
 *   ls --help
 *   cat --help file.txt
 *   echo --help
 *   edit --help file.txt
 *
 * To add a new command, use the parse_command helper to extract the command, flag, and arguments.
 *
 * Helper: parse_command
 *   void parse_command(const char* cmd, char* out_cmd, char* out_args, char* out_flag);
 *   - Extracts the first word as the command, the first flag (if present, e.g. --help), and the rest as arguments.
 *   - out_cmd: receives the command name
 *   - out_flag: receives the flag (e.g. --help, -h), or empty string if none
 *   - out_args: receives the rest of the arguments (may be empty)
 */
#include "shell.h"
#include "../drivers/console.h"
#include "../fs/filesystem.h"
#include "../editor/editor.h"
#include "../drivers/keyboard.h"
#include "../commands/command_utils.h"
#include "../commands/help.h"
#include "../commands/clear.h"
#include "../commands/count.h"
#include "../commands/ls.h"
#include "../commands/cat.h"
#include "../commands/echo.h"
#include "../commands/edit.h"

// Helper to parse command, arguments, and flags
static void parse_command(const char* cmd, char* out_cmd, char* out_args, char* out_flag) {
    // Extract command (first word)
    int i = 0, j = 0;
    while (cmd[i] && cmd[i] != ' ' && cmd[i] != '\t') {
        out_cmd[j++] = cmd[i++];
    }
    out_cmd[j] = 0;
    // Skip whitespace
    while (cmd[i] == ' ' || cmd[i] == '\t') i++;
    // Check for flag (starts with '-')
    if (cmd[i] == '-') {
        int k = 0;
        while (cmd[i] && cmd[i] != ' ' && cmd[i] != '\t') {
            out_flag[k++] = cmd[i++];
        }
        out_flag[k] = 0;
        while (cmd[i] == ' ' || cmd[i] == '\t') i++;
    } else {
        out_flag[0] = 0;
    }
    // The rest is args
    int k = 0;
    while (cmd[i]) out_args[k++] = cmd[i++];
    out_args[k] = 0;
}

void handle_command(const char* cmd) {
    // Trim leading whitespace
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    if (cmd[0] == 0) {
        print_prompt();
        return;
    }
    char command[32] = {0};
    char args[128] = {0};
    char flag[32] = {0};
    parse_command(cmd, command, args, flag);
    if (!kstrcmp(command, "help") || !kstrcmp(command, "--help")) {
        cmd_help(args, flag);
        print_prompt();
        return;
    }
    if (!kstrcmp(command, "clear")) {
        cmd_clear(args, flag);
        print_prompt();
        return;
    }
    if (!kstrcmp(command, "count")) {
        cmd_count(args, flag);
        print_prompt();
        return;
    }
    if (!kstrcmp(command, "ls")) {
        cmd_ls(args, flag);
        print_prompt();
        return;
    }
    if (!kstrcmp(command, "cat")) {
        cmd_cat(args, flag);
        print_prompt();
        return;
    }
    if (!kstrcmp(command, "echo")) {
        cmd_echo(args, flag);
        print_prompt();
        return;
    }
    if (!kstrcmp(command, "edit")) {
        cmd_edit(args, flag);
        print_prompt();
        return;
    }
    print("Unknown command: ");
    print(command);
    print("\n");
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