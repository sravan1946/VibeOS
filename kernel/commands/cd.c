#include "cd.h"
#include "../drivers/console.h"
#include "../fs/filesystem.h"
#include "../drivers/graphics.h"
#include "command_utils.h"

void cmd_cd(const char* args, const char* flag) {
    if (flag && (!kstrcmp(flag, "--help") || !kstrcmp(flag, "-h"))) {
        graphics_console_set_color(rgb16(0,255,128), rgb16(0,0,0)); // bright green for usage
        print("Usage: cd ");
        graphics_console_set_color(rgb16(255,128,0), rgb16(0,0,0)); // orange for args
        print("<dir>\n");
        graphics_console_set_color(rgb16(220,220,220), rgb16(0,0,0)); // soft white for description
        print("Changes the current working directory.\n");
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
    if (fs_chdir(dir)) {
        graphics_console_set_color(rgb16(0,255,0), rgb16(0,0,0)); // green for success
        print("Changed directory.\n");
        graphics_console_set_color(rgb16(220,220,220), rgb16(0,0,0));
    } else {
        graphics_console_set_color(rgb16(255,64,64), rgb16(0,0,0)); // bright red for error
        print("No such directory.\n");
        graphics_console_set_color(rgb16(220,220,220), rgb16(0,0,0));
    }
} 