#include "rm.h"
#include "../drivers/console.h"
#include "../fs/filesystem.h"
#include "../drivers/graphics.h"
#include "command_utils.h"

void cmd_rm(const char* args, const char* flag) {
    if (flag && (!kstrcmp(flag, "--help") || !kstrcmp(flag, "-h"))) {
        graphics_console_set_color(rgb16(0,255,128), rgb16(0,0,0)); // bright green for usage
        print("Usage: rm ");
        graphics_console_set_color(rgb16(255,128,0), rgb16(0,0,0)); // orange for args
        print("<file|dir>\n");
        graphics_console_set_color(rgb16(220,220,220), rgb16(0,0,0)); // soft white for description
        print("Removes a file or an empty directory.\n");
        return;
    }
    const char* path = args;
    while (*path == ' ' || *path == '\t') path++;
    if (!*path) { 
        graphics_console_set_color(rgb16(255,64,64), rgb16(0,0,0)); // bright red for error
        print("No file or directory specified.\n");
        graphics_console_set_color(rgb16(220,220,220), rgb16(0,0,0));
        return; 
    }
    if (fs_remove(path)) {
        graphics_console_set_color(rgb16(0,255,0), rgb16(0,0,0)); // green for success
        print("Removed: "); print(path); print("\n");
        graphics_console_set_color(rgb16(220,220,220), rgb16(0,0,0));
    } else {
        graphics_console_set_color(rgb16(255,64,64), rgb16(0,0,0)); // bright red for error
        print("Failed to remove (not found or not empty).\n");
        graphics_console_set_color(rgb16(220,220,220), rgb16(0,0,0));
    }
} 