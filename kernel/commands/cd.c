#include "cd.h"
#include "../drivers/console.h"
#include "../fs/filesystem.h"
#include "../drivers/graphics.h"
#include "command_utils.h"

void cmd_cd(const char* args, const char* flag) {
    if (flag && (!kstrcmp(flag, "--help") || !kstrcmp(flag, "-h"))) {
        graphics_console_set_color(10, 0); // green for usage
        print("Usage: cd ");
        graphics_console_set_color(13, 0); // magenta for args
        print("<dir>\n");
        graphics_console_set_color(15, 0); // white for description
        print("Changes the current working directory.\n");
        return;
    }
    const char* dir = args;
    while (*dir == ' ' || *dir == '\t') dir++;
    if (!*dir) { print("No directory specified.\n"); return; }
    if (fs_chdir(dir)) {
        // Success, print nothing
    } else {
        print("No such directory.\n");
    }
} 