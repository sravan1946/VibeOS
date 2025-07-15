#include "echo.h"
#include "../drivers/console.h"
#include "../fs/filesystem.h"
#include "../drivers/graphics.h"
#include "command_utils.h"

void cmd_echo(const char* args, const char* flag) {
    if (flag && (!kstrcmp(flag, "--help") || !kstrcmp(flag, "-h"))) {
        graphics_console_set_color(rgb16(0,255,128), rgb16(0,0,0)); // bright green for usage
        print("Usage: echo ");
        graphics_console_set_color(rgb16(255,128,0), rgb16(0,0,0)); // orange for args
        print("<text>");
        graphics_console_set_color(rgb16(220,220,220), rgb16(0,0,0)); // soft white for description
        print("\nPrints text to the screen.\nYou can also use: echo <text> > <file> to write to a file, or echo <text> >> <file> to append.\n");
        return;
    }
    const char* gt = 0;
    for (int i = 0; args[i]; i++) {
        if (args[i] == '>' && args[i+1] == ' ') { gt = args + i; break; }
    }
    if (gt) {
        int text_len = gt - args;
        char text[MAX_FILESIZE];
        for (int i = 0; i < text_len && i < MAX_FILESIZE; i++) text[i] = args[i];
        const char* fname = gt + 2;
        while (*fname == ' ' || *fname == '\t') fname++;
        if (fs_write(fname, text, text_len)) {
            graphics_console_set_color(rgb16(0,255,0), rgb16(0,0,0)); // green for success
            print("Wrote file: "); print(fname); print("\n");
        } else {
            graphics_console_set_color(rgb16(255,64,64), rgb16(0,0,0)); // bright red for error
            print("File write error.\n");
        }
    } else {
        graphics_console_set_color(rgb16(255,255,255), rgb16(0,0,0));
        print(args);
        print("\n");
    }
} 