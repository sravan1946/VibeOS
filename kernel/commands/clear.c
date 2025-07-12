#include "clear.h"
#include "../drivers/console.h"
#include "../drivers/graphics.h"
#include "command_utils.h"

void cmd_clear(const char* args, const char* flag) {
    (void)args;
    if (flag && (!kstrcmp(flag, "--help") || !kstrcmp(flag, "-h"))) {
        graphics_console_set_color(10, 0); // green for usage
        print("Usage: clear\n");
        graphics_console_set_color(15, 0); // white for description
        print("Clears the screen.\n");
        return;
    }
    clear_screen();
    print("VibeOS Shell\n\n");
} 