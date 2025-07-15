#include "clear.h"
#include "../drivers/console.h"
#include "../drivers/graphics.h"
#include "command_utils.h"

void cmd_clear(const char* args, const char* flag) {
    (void)args;
    if (flag && (!kstrcmp(flag, "--help") || !kstrcmp(flag, "-h"))) {
        graphics_console_set_color(rgb16(0,255,128), rgb16(0,0,0)); // bright green for usage
        print("Usage: clear\n");
        graphics_console_set_color(rgb16(220,220,220), rgb16(0,0,0)); // soft white for description
        print("Clears the screen.\n");
        return;
    }
    clear_screen();
    graphics_console_set_color(rgb16(0,255,255), rgb16(0,0,0)); // cyan for shell banner
    print("VibeOS Shell\n\n");
    graphics_console_set_color(rgb16(220,220,220), rgb16(0,0,0));
} 