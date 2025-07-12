#include "touch.h"
#include "../drivers/console.h"
#include "../fs/filesystem.h"
#include "../drivers/graphics.h"
#include "command_utils.h"

void cmd_touch(const char* args, const char* flag) {
    if (flag && (!kstrcmp(flag, "--help") || !kstrcmp(flag, "-h"))) {
        graphics_console_set_color(10, 0); // green for usage
        print("Usage: touch ");
        graphics_console_set_color(13, 0); // magenta for args
        print("<file>\n");
        graphics_console_set_color(15, 0); // white for description
        print("Creates an empty file or updates its timestamp (not implemented).\n");
        return;
    }
    const char* fname = args;
    while (*fname == ' ' || *fname == '\t') fname++;
    if (!*fname) { print("No file specified.\n"); return; }
    if (fs_write(fname, "", 0)) {
        print("Touched file: "); print(fname); print("\n");
    } else {
        print("Failed to touch file.\n");
    }
} 