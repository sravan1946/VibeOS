#include "edit.h"
#include "../drivers/console.h"
#include "../editor/editor.h"
#include "../drivers/graphics.h"
#include "command_utils.h"

void cmd_edit(const char* args, const char* flag) {
    if (flag && (!kstrcmp(flag, "--help") || !kstrcmp(flag, "-h"))) {
        graphics_console_set_color(rgb16(0,255,128), rgb16(0,0,0)); // bright green for usage
        print("Usage: edit ");
        graphics_console_set_color(rgb16(255,128,0), rgb16(0,0,0)); // orange for args
        print("<file>\n");
        graphics_console_set_color(rgb16(220,220,220), rgb16(0,0,0)); // soft white for description
        print("Edits a file.\n");
        return;
    }
    const char* fname = args;
    while (*fname == ' ' || *fname == '\t') fname++;
    editor(fname);
} 