#include "clear.h"
#include "../drivers/console.h"
#include "command_utils.h"

void cmd_clear(const char* args, const char* flag) {
    (void)args;
    if (flag && (!kstrcmp(flag, "--help") || !kstrcmp(flag, "-h"))) {
        print("Usage: clear [--help]\nClears the screen.\n");
        return;
    }
    clear_screen();
    print("VibeOS Shell\n\n");
} 