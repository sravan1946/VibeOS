#include "help.h"
#include "../drivers/console.h"
#include "../drivers/graphics.h"
#include "command_utils.h"

// Forward declarations for per-command help stubs
static void help_clear(void);
static void help_count(void);
static void help_echo(void);
static void help_ls(void);
static void help_cat(void);
static void help_edit(void);
static void help_pwd(void);
static void help_cd(void);
static void help_mkdir(void);
static void help_rm(void);
static void help_touch(void);
static void help_help(void);

static int kstrlen(const char* s) { int i = 0; while (s[i]) i++; return i; }

// Color helpers
#define COL_CMD   rgb16(255,255,0)    // yellow
#define COL_ARG   rgb16(255,128,0)    // orange
#define COL_SYM   rgb16(220,64,255)   // magenta
#define COL_DESC  rgb16(220,220,220)  // soft white
#define COL_HEAD  rgb16(0,255,255)    // cyan
#define COL_USAGE rgb16(0,255,128)    // green
#define COL_REDIR rgb16(0,180,255)    // blue
#define COL_ERR   rgb16(255,64,64)    // bright red

// Print argument string with symbols colored
static void print_args(const char* arg) {
    graphics_console_set_color(COL_ARG, 0);
    for (int i = 0; arg[i]; i++) {
        char c = arg[i];
        if (c == '<' || c == '>' || c == '|' || c == '[' || c == ']') {
            graphics_console_set_color(COL_SYM, 0);
            print_char(c);
            graphics_console_set_color(COL_ARG, 0);
        } else {
            print_char(c);
        }
    }
}

// Print description with minimal color (only command names and redirection)
static void print_desc(const char* desc, const char* cmd) {
    while (*desc) {
        int cmdlen = kstrlen(cmd);
        if (cmdlen > 0 && !kstrncmp(desc, cmd, cmdlen)) {
            graphics_console_set_color(COL_USAGE, 0);
            print(cmd);
            desc += cmdlen;
            graphics_console_set_color(COL_DESC, 0);
        } else if (*desc == '>' && *(desc+1) == '>') {
            graphics_console_set_color(COL_REDIR, 0);
            print(">>");
            graphics_console_set_color(COL_DESC, 0);
            desc += 2;
        } else if (*desc == '>' && (desc == desc || *(desc-1) == ' ')) {
            graphics_console_set_color(COL_REDIR, 0);
            print_char('>');
            graphics_console_set_color(COL_DESC, 0);
            desc++;
        } else {
            print_char(*desc);
            desc++;
        }
    }
}

// Main help table
void cmd_help(const char* args, const char* flag) {
    // If --help, show help for help
    if (flag && (!kstrcmp(flag, "--help") || !kstrcmp(flag, "-h"))) {
        graphics_console_set_color(COL_USAGE, 0);
        print("Usage: help [command]\n");
        graphics_console_set_color(COL_DESC, 0);
        print("Shows this help.\n");
        return;
    }
    // If 'help <command>', show that command's help
    while (*args == ' ' || *args == '\t') args++;
    if (*args) {
        char cmd[16] = {0};
        int i = 0;
        while (args[i] && args[i] != ' ' && args[i] != '\t' && i < 15) { cmd[i] = args[i]; i++; }
        cmd[i] = 0;
        if (!kstrcmp(cmd, "clear")) { help_clear(); return; }
        if (!kstrcmp(cmd, "count")) { help_count(); return; }
        if (!kstrcmp(cmd, "echo")) { help_echo(); return; }
        if (!kstrcmp(cmd, "ls"))   { help_ls();   return; }
        if (!kstrcmp(cmd, "cat"))  { help_cat();  return; }
        if (!kstrcmp(cmd, "edit")) { help_edit(); return; }
        if (!kstrcmp(cmd, "pwd"))  { help_pwd();  return; }
        if (!kstrcmp(cmd, "cd"))   { help_cd();   return; }
        if (!kstrcmp(cmd, "mkdir")){ help_mkdir();return; }
        if (!kstrcmp(cmd, "rm"))   { help_rm();   return; }
        if (!kstrcmp(cmd, "touch")){ help_touch();return; }
        if (!kstrcmp(cmd, "help")) { help_help(); return; }
        graphics_console_set_color(COL_ERR, 0);
        print("No such command: "); print(cmd); print("\n");
        graphics_console_set_color(COL_DESC, 0);
        return;
    }
    // Main help table
    graphics_console_set_color(COL_HEAD, 0); print("Available commands:\n\n");
    // Table header
    graphics_console_set_color(COL_HEAD, 0); print("  Command     Arguments         Description\n");
    graphics_console_set_color(COL_HEAD, 0); print("  -------     ---------         -----------\n");
    // Table rows
    #define ROW(cmd, args, desc) \
        graphics_console_set_color(COL_CMD, 0); print("  " cmd); \
        for (int i = kstrlen(cmd)+2; i < 13; i++) print(" "); \
        print_args(args); \
        for (int i = kstrlen(args); i < 17; i++) print(" "); \
        graphics_console_set_color(COL_DESC, 0); print(desc); print("\n");
    ROW("help",   "[command]",   "Shows this help");
    ROW("clear",  "",           "Clears the screen");
    ROW("count",  "",           "Shows interrupt counts");
    ROW("echo",   "<text>",     "Prints text, > or >> to file");
    ROW("ls",     "[dir]",      "Lists files");
    ROW("cat",    "<file>",     "Shows file contents");
    ROW("edit",   "<file>",     "Edits a file");
    ROW("pwd",    "",           "Shows current working directory");
    ROW("cd",     "<dir>",      "Changes directory");
    ROW("mkdir",  "<dir>",      "Creates directory");
    ROW("rm",     "<file|dir>", "Removes file or empty directory");
    ROW("touch",  "<file>",     "Creates an empty file");
    #undef ROW
    graphics_console_set_color(COL_DESC, 0);
    print("\nType ");
    graphics_console_set_color(COL_USAGE, 0); print("help <command>");
    graphics_console_set_color(COL_DESC, 0); print(" for details.\n");
}

// --- Per-command help stubs: Modern, clear, and consistent ---
static void help_clear(void) {
    graphics_console_set_color(COL_HEAD, 0); print("clear - Clear the screen\n\n");
    graphics_console_set_color(COL_USAGE, 0); print("Usage: clear\n\n");
    graphics_console_set_color(COL_DESC, 0); print("Clears the screen and resets the shell display.\n");
}
static void help_count(void) {
    graphics_console_set_color(COL_HEAD, 0); print("count - Show interrupt counts\n\n");
    graphics_console_set_color(COL_USAGE, 0); print("Usage: count\n\n");
    graphics_console_set_color(COL_DESC, 0); print("Displays the number of timer and keyboard interrupts since boot.\n");
}
static void help_echo(void) {
    graphics_console_set_color(COL_HEAD, 0); print("echo - Print text or write to file\n\n");
    graphics_console_set_color(COL_USAGE, 0); print("Usage: echo "); print_args("<text>"); print("\n\n");
    graphics_console_set_color(COL_DESC, 0); print("Prints text to the screen. You can also redirect output to a file.\n\n");
    graphics_console_set_color(COL_DESC, 0); print("Examples:\n");
    graphics_console_set_color(COL_USAGE, 0); print("  echo "); graphics_console_set_color(COL_ARG, 0); print("hello world"); graphics_console_set_color(COL_DESC, 0); print("\n");
    graphics_console_set_color(COL_USAGE, 0); print("  echo "); graphics_console_set_color(COL_ARG, 0); print("hello > "); graphics_console_set_color(COL_REDIR, 0); print("file.txt"); graphics_console_set_color(COL_DESC, 0); print("\n");
    graphics_console_set_color(COL_USAGE, 0); print("  echo "); graphics_console_set_color(COL_ARG, 0); print("hi >> "); graphics_console_set_color(COL_REDIR, 0); print("file.txt"); graphics_console_set_color(COL_DESC, 0); print("\n");
}
static void help_ls(void) {
    graphics_console_set_color(COL_HEAD, 0); print("ls - List files\n\n");
    graphics_console_set_color(COL_USAGE, 0); print("Usage: ls "); print_args("[dir]"); print("\n\n");
    graphics_console_set_color(COL_DESC, 0); print("Lists all files and directories in the specified directory (default: current directory).\n");
    graphics_console_set_color(COL_DESC, 0); print("Example:\n");
    graphics_console_set_color(COL_USAGE, 0); print("  ls\n  ls "); graphics_console_set_color(COL_ARG, 0); print("docs\n");
}
static void help_cat(void) {
    graphics_console_set_color(COL_HEAD, 0); print("cat - Show file contents\n\n");
    graphics_console_set_color(COL_USAGE, 0); print("Usage: cat "); print_args("<file>"); print("\n\n");
    graphics_console_set_color(COL_DESC, 0); print("Displays the contents of a file.\n");
    graphics_console_set_color(COL_DESC, 0); print("Example:\n");
    graphics_console_set_color(COL_USAGE, 0); print("  cat "); graphics_console_set_color(COL_ARG, 0); print("readme.txt\n");
}
static void help_edit(void) {
    graphics_console_set_color(COL_HEAD, 0); print("edit - Edit a file\n\n");
    graphics_console_set_color(COL_USAGE, 0); print("Usage: edit "); print_args("<file>"); print("\n\n");
    graphics_console_set_color(COL_DESC, 0); print("Opens a simple text editor for the specified file.\n");
    graphics_console_set_color(COL_DESC, 0); print("Example:\n");
    graphics_console_set_color(COL_USAGE, 0); print("  edit "); graphics_console_set_color(COL_ARG, 0); print("notes.txt\n");
}
static void help_pwd(void) {
    graphics_console_set_color(COL_HEAD, 0); print("pwd - Print working directory\n\n");
    graphics_console_set_color(COL_USAGE, 0); print("Usage: pwd\n\n");
    graphics_console_set_color(COL_DESC, 0); print("Shows the current working directory path.\n");
}
static void help_cd(void) {
    graphics_console_set_color(COL_HEAD, 0); print("cd - Change directory\n\n");
    graphics_console_set_color(COL_USAGE, 0); print("Usage: cd "); print_args("<dir>"); print("\n\n");
    graphics_console_set_color(COL_DESC, 0); print("Changes the current working directory.\n");
    graphics_console_set_color(COL_DESC, 0); print("Example:\n");
    graphics_console_set_color(COL_USAGE, 0); print("  cd "); graphics_console_set_color(COL_ARG, 0); print("docs\n");
}
static void help_mkdir(void) {
    graphics_console_set_color(COL_HEAD, 0); print("mkdir - Make directory\n\n");
    graphics_console_set_color(COL_USAGE, 0); print("Usage: mkdir "); print_args("<dir>"); print("\n\n");
    graphics_console_set_color(COL_DESC, 0); print("Creates a new directory.\n");
    graphics_console_set_color(COL_DESC, 0); print("Example:\n");
    graphics_console_set_color(COL_USAGE, 0); print("  mkdir "); graphics_console_set_color(COL_ARG, 0); print("projects\n");
}
static void help_rm(void) {
    graphics_console_set_color(COL_HEAD, 0); print("rm - Remove file or directory\n\n");
    graphics_console_set_color(COL_USAGE, 0); print("Usage: rm "); print_args("<file|dir>"); print("\n\n");
    graphics_console_set_color(COL_DESC, 0); print("Removes a file or an empty directory.\n");
    graphics_console_set_color(COL_DESC, 0); print("Example:\n");
    graphics_console_set_color(COL_USAGE, 0); print("  rm "); graphics_console_set_color(COL_ARG, 0); print("old.txt\n");
}
static void help_touch(void) {
    graphics_console_set_color(COL_HEAD, 0); print("touch - Create empty file\n\n");
    graphics_console_set_color(COL_USAGE, 0); print("Usage: touch "); print_args("<file>"); print("\n\n");
    graphics_console_set_color(COL_DESC, 0); print("Creates an empty file or updates its timestamp.\n");
    graphics_console_set_color(COL_DESC, 0); print("Example:\n");
    graphics_console_set_color(COL_USAGE, 0); print("  touch "); graphics_console_set_color(COL_ARG, 0); print("newfile.txt\n");
}
static void help_help(void) {
    graphics_console_set_color(COL_HEAD, 0); print("help - Show this help\n\n");
    graphics_console_set_color(COL_USAGE, 0); print("Usage: help\n\n");
    graphics_console_set_color(COL_DESC, 0); print("Shows this help.\n");
} 