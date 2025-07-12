#include "mkdir.h"
#include "../drivers/console.h"
#include "../fs/filesystem.h"
#include "../drivers/graphics.h"
#include "command_utils.h"

void cmd_mkdir(const char* args, const char* flag) {
    if (flag && (!kstrcmp(flag, "--help") || !kstrcmp(flag, "-h"))) {
        graphics_console_set_color(10, 0); // green for usage
        print("Usage: mkdir ");
        graphics_console_set_color(13, 0); // magenta for args
        print("<dir>\n");
        graphics_console_set_color(15, 0); // white for description
        print("Creates a new directory.\n");
        return;
    }
    const char* dir = args;
    while (*dir == ' ' || *dir == '\t') dir++;
    if (!*dir) { print("No directory specified.\n"); return; }
    if (fs_mkdir(dir)) {
        print("Created directory: "); print(dir); print("\n");
    } else {
        print("Failed to create directory.\n");
    }
} 