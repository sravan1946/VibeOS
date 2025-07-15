#include "mkdir.h"
#include "../drivers/console.h"
#include "../fs/filesystem.h"
#include "../drivers/graphics.h"
#include "command_utils.h"

void cmd_mkdir(const char* args, const char* flag) {
    if (flag && (!kstrcmp(flag, "--help") || !kstrcmp(flag, "-h"))) {
        graphics_console_set_color(rgb16(0,255,128), rgb16(0,0,0)); // bright green for usage
        print("Usage: mkdir ");
        graphics_console_set_color(rgb16(255,128,0), rgb16(0,0,0)); // orange for args
        print("<dir>\n");
        graphics_console_set_color(rgb16(220,220,220), rgb16(0,0,0)); // soft white for description
        print("Creates a new directory.\n");
        return;
    }
    const char* dir = args;
    while (*dir == ' ' || *dir == '\t') dir++;
    if (!*dir) {
        graphics_console_set_color(rgb16(255,64,64), rgb16(0,0,0)); // bright red for error
        print("No directory specified.\n");
        graphics_console_set_color(rgb16(220,220,220), rgb16(0,0,0));
        return;
    }
    if (fs_mkdir(dir)) {
        graphics_console_set_color(rgb16(0,255,0), rgb16(0,0,0)); // green for success
        print("Created directory: "); print(dir); print("\n");
        graphics_console_set_color(rgb16(220,220,220), rgb16(0,0,0));
    } else {
        graphics_console_set_color(rgb16(255,64,64), rgb16(0,0,0)); // bright red for error
        print("Failed to create directory.\n");
        graphics_console_set_color(rgb16(220,220,220), rgb16(0,0,0));
    }
} 