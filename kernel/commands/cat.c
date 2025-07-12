#include "cat.h"
#include "../drivers/console.h"
#include "../fs/filesystem.h"
#include "../drivers/graphics.h"
#include "command_utils.h"

void cmd_cat(const char* args, const char* flag) {
    if (flag && (!kstrcmp(flag, "--help") || !kstrcmp(flag, "-h"))) {
        graphics_console_set_color(10, 0); // green for usage
        print("Usage: cat ");
        graphics_console_set_color(13, 0); // magenta for args
        print("<file>\n");
        graphics_console_set_color(15, 0); // white for description
        print("Shows the contents of a file.\n");
        return;
    }
    const char* fname = args;
    while (*fname == ' ' || *fname == '\t') fname++;
    int len = 0;
    const char* data = fs_read(fname, &len);
    if (!data) {
        print("File not found.\n");
    } else {
        for (int i = 0; i < len; i++) print_char(data[i]);
        print("\n");
    }
} 