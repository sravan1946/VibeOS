#include "edit.h"
#include "../drivers/console.h"
#include "../editor/editor.h"
#include "../drivers/graphics.h"
#include "command_utils.h"

void cmd_edit(const char* args, const char* flag) {
    if (flag && (!kstrcmp(flag, "--help") || !kstrcmp(flag, "-h"))) {
        graphics_console_set_color(10, 0); // green for usage
        print("Usage: edit ");
        graphics_console_set_color(13, 0); // magenta for args
        print("<file>\n");
        graphics_console_set_color(15, 0); // white for description
        print("Edits a file.\n");
        return;
    }
    const char* fname = args;
    while (*fname == ' ' || *fname == '\t') fname++;
    editor(fname);
} 