#include "help.h"
#include "../drivers/console.h"
#include "command_utils.h"

void cmd_help(const char* args, const char* flag) {
    (void)args; // Not used for now
    if (flag && (!kstrcmp(flag, "--help") || !kstrcmp(flag, "-h"))) {
        print("Usage: help [command] [--help]\nShows help for a command.\n");
        return;
    }
    print("Available commands:\n");
    print("  help [command] [--help]   - Show help for a command\n");
    print("  clear [--help]            - Clear the screen\n");
    print("  count [--help]            - Show interrupt counts\n");
    print("  echo [--help] <text>      - Print text\n");
    print("  ls [--help]               - List files\n");
    print("  cat [--help] <file>       - Show file contents\n");
    print("  echo <text> > <file>      - Write file\n");
    print("  edit [--help] <file>      - Edit a file\n");
} 