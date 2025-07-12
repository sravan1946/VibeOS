#include "help.h"
#include "../drivers/console.h"
#include "../drivers/graphics.h"
#include "command_utils.h"

static int kstrlen(const char* s) { int i = 0; while (s[i]) i++; return i; }

typedef void (*help_func_t)(void);

// Forward declarations for help stubs
static void help_clear(void); static void help_count(void); static void help_echo(void); static void help_ls(void);
static void help_cat(void); static void help_edit(void); static void help_pwd(void); static void help_cd(void);
static void help_mkdir(void); static void help_rm(void); static void help_touch(void);

void cmd_help(const char* args, const char* flag) {
    // If --help, show help for help
    if (flag && (!kstrcmp(flag, "--help") || !kstrcmp(flag, "-h"))) {
        graphics_console_set_color(10, 0); // green for usage
        print("Usage: help [command]\n");
        graphics_console_set_color(15, 0); // white for description
        print("Shows this help.\n");
        return;
    }
    // If 'help <command>', show that command's help
    while (*args == ' ' || *args == '\t') args++;
    if (*args) {
        // Only match the first word (command)
        char cmd[16] = {0};
        int i = 0;
        while (args[i] && args[i] != ' ' && args[i] != '\t' && i < 15) { cmd[i] = args[i]; i++; }
        cmd[i] = 0;
        // Map command to help stub
        struct { const char* name; help_func_t func; } table[] = {
            {"clear", help_clear}, {"count", help_count}, {"echo", help_echo}, {"ls", help_ls},
            {"cat", help_cat}, {"edit", help_edit}, {"pwd", help_pwd}, {"cd", help_cd},
            {"mkdir", help_mkdir}, {"rm", help_rm}, {"touch", help_touch},
        };
        int found = 0;
        for (unsigned j = 0; j < sizeof(table)/sizeof(table[0]); j++) {
            if (!kstrcmp(cmd, table[j].name)) { table[j].func(); found = 1; break; }
        }
        if (!found) {
            graphics_console_set_color(12, 0); // red
            print("No such command: "); print(cmd); print("\n");
            graphics_console_set_color(15, 0);
        }
        return;
    }
    // Main help output
    graphics_console_set_color(11, 0); // cyan header
    print("Available commands:\n");
    #define HELP_LINE(cmd, flags, desc, ccol, fcol) \
        graphics_console_set_color(14, 0); print(cmd); \
        for (int i = kstrlen(cmd); i < ccol; i++) print(" "); \
        graphics_console_set_color(13, 0); print(flags); \
        for (int i = kstrlen(flags); i < fcol; i++) print(" "); \
        graphics_console_set_color(15, 0); print(desc); print("\n");
    HELP_LINE("  help", "[command]", "Shows this help", 12, 20);
    HELP_LINE("  clear", "", "Clears the screen", 12, 20);
    HELP_LINE("  count", "", "Shows interrupt counts", 12, 20);
    HELP_LINE("  echo", "<text>", "Prints text, > or >> to file", 12, 20);
    HELP_LINE("  ls", "[dir]", "Lists files", 12, 20);
    HELP_LINE("  cat", "<file>", "Shows file contents", 12, 20);
    HELP_LINE("  edit", "<file>", "Edits a file", 12, 20);
    HELP_LINE("  pwd", "", "Shows current working directory", 12, 20);
    HELP_LINE("  cd", "<dir>", "Changes directory", 12, 20);
    HELP_LINE("  mkdir", "<dir>", "Creates directory", 12, 20);
    HELP_LINE("  rm", "<file|dir>", "Removes file or empty directory", 12, 20);
    HELP_LINE("  touch", "<file>", "Creates an empty file", 12, 20);
    graphics_console_set_color(15, 0); // restore white on black
    #undef HELP_LINE
}

// Per-command help stubs (mimic --help output)
static void help_clear(void) {
    graphics_console_set_color(10, 0); print("Usage: clear\n");
    graphics_console_set_color(15, 0); print("Clears the screen.\n");
}
static void help_count(void) {
    graphics_console_set_color(10, 0); print("Usage: count\n");
    graphics_console_set_color(15, 0); print("Shows the number of timer and keyboard interrupts.\n");
}
static void help_echo(void) {
    graphics_console_set_color(10, 0); print("Usage: echo ");
    graphics_console_set_color(13, 0); print("<text>\n");
    graphics_console_set_color(15, 0); print("Prints text to the screen.\nYou can also use: echo <text> > <file> to write to a file, or echo <text> >> <file> to append.\n");
}
static void help_ls(void) {
    graphics_console_set_color(10, 0); print("Usage: ls ");
    graphics_console_set_color(13, 0); print("[dir]\n");
    graphics_console_set_color(15, 0); print("Lists all files and directories in the given directory (default: current).\n");
}
static void help_cat(void) {
    graphics_console_set_color(10, 0); print("Usage: cat ");
    graphics_console_set_color(13, 0); print("<file>\n");
    graphics_console_set_color(15, 0); print("Shows the contents of a file.\n");
}
static void help_edit(void) {
    graphics_console_set_color(10, 0); print("Usage: edit ");
    graphics_console_set_color(13, 0); print("<file>\n");
    graphics_console_set_color(15, 0); print("Edits a file.\n");
}
static void help_pwd(void) {
    graphics_console_set_color(10, 0); print("Usage: pwd\n");
    graphics_console_set_color(15, 0); print("Shows the current working directory.\n");
}
static void help_cd(void) {
    graphics_console_set_color(10, 0); print("Usage: cd ");
    graphics_console_set_color(13, 0); print("<dir>\n");
    graphics_console_set_color(15, 0); print("Changes the current working directory.\n");
}
static void help_mkdir(void) {
    graphics_console_set_color(10, 0); print("Usage: mkdir ");
    graphics_console_set_color(13, 0); print("<dir>\n");
    graphics_console_set_color(15, 0); print("Creates a new directory.\n");
}
static void help_rm(void) {
    graphics_console_set_color(10, 0); print("Usage: rm ");
    graphics_console_set_color(13, 0); print("<file|dir>\n");
    graphics_console_set_color(15, 0); print("Removes a file or an empty directory.\n");
}
static void help_touch(void) {
    graphics_console_set_color(10, 0); print("Usage: touch ");
    graphics_console_set_color(13, 0); print("<file>\n");
    graphics_console_set_color(15, 0); print("Creates an empty file or updates its timestamp (not implemented).\n");
} 