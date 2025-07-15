#include "touch.h"
#include "../drivers/console.h"
#include "../fs/filesystem.h"
#include "../drivers/graphics.h"
#include "command_utils.h"

void cmd_touch(const char* args, const char* flag) {
    if (flag && (!kstrcmp(flag, "--help") || !kstrcmp(flag, "-h"))) {
        graphics_console_set_color(rgb16(0,255,128), rgb16(0,0,0)); // bright green for usage
        print("Usage: touch ");
        graphics_console_set_color(rgb16(255,128,0), rgb16(0,0,0)); // orange for args
        print("<file>\n");
        graphics_console_set_color(rgb16(220,220,220), rgb16(0,0,0)); // soft white for description
        print("Creates an empty file or updates its timestamp (not implemented).\n");
        return;
    }
    const char* fname = args;
    while (*fname == ' ' || *fname == '\t') fname++;
    if (!*fname) { 
        graphics_console_set_color(rgb16(255,64,64), rgb16(0,0,0)); // bright red for error
        print("No file specified.\n");
        graphics_console_set_color(rgb16(220,220,220), rgb16(0,0,0));
        return; 
    }
    if (fs_write(fname, "", 0)) {
        graphics_console_set_color(rgb16(0,255,0), rgb16(0,0,0)); // green for success
        print("Touched file: "); print(fname); print("\n");
        graphics_console_set_color(rgb16(220,220,220), rgb16(0,0,0));
    } else {
        graphics_console_set_color(rgb16(255,64,64), rgb16(0,0,0)); // bright red for error
        print("Failed to touch file.\n");
        graphics_console_set_color(rgb16(220,220,220), rgb16(0,0,0));
    }
} 