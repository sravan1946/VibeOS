#include "rm.h"
#include "../drivers/console.h"
#include "../fs/filesystem.h"
#include "../drivers/graphics.h"
#include "command_utils.h"

void cmd_rm(const char* args, const char* flag) {
    if (flag && (!kstrcmp(flag, "--help") || !kstrcmp(flag, "-h"))) {
        graphics_console_set_color(10, 0); // green for usage
        print("Usage: rm ");
        graphics_console_set_color(13, 0); // magenta for args
        print("<file|dir>\n");
        graphics_console_set_color(15, 0); // white for description
        print("Removes a file or an empty directory.\n");
        return;
    }
    const char* path = args;
    while (*path == ' ' || *path == '\t') path++;
    if (!*path) { print("No file or directory specified.\n"); return; }
    if (fs_remove(path)) {
        print("Removed: "); print(path); print("\n");
    } else {
        print("Failed to remove (not found or not empty).\n");
    }
} 