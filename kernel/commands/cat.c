#include "cat.h"
#include "../drivers/console.h"
#include "../fs/filesystem.h"
#include "../drivers/graphics.h"
#include "command_utils.h"

void cmd_cat(const char* args, const char* flag) {
    if (flag && (!kstrcmp(flag, "--help") || !kstrcmp(flag, "-h"))) {
        graphics_console_set_color(rgb16(0,255,128), rgb16(0,0,0)); // bright green for usage
        print("Usage: cat ");
        graphics_console_set_color(rgb16(255,128,0), rgb16(0,0,0)); // orange for args
        print("<file>\n");
        graphics_console_set_color(rgb16(220,220,220), rgb16(0,0,0)); // soft white for description
        print("Shows the contents of a file.\n");
        return;
    }
    const char* fname = args;
    while (*fname == ' ' || *fname == '\t') fname++;
    int len = 0;
    const char* data = fs_read(fname, &len);
    if (!data) {
        graphics_console_set_color(rgb16(255,64,64), rgb16(0,0,0)); // bright red for error
        print("File not found.\n");
        graphics_console_set_color(rgb16(220,220,220), rgb16(0,0,0));
    } else {
        graphics_console_set_color(rgb16(255,255,255), rgb16(0,0,0));
        for (int i = 0; i < len; i++) print_char(data[i]);
        print("\n");
    }
} 