#include "echo.h"
#include "../drivers/console.h"
#include "../fs/filesystem.h"
#include "../drivers/graphics.h"
#include "command_utils.h"

void cmd_echo(const char* args, const char* flag) {
    if (flag && (!kstrcmp(flag, "--help") || !kstrcmp(flag, "-h"))) {
        graphics_console_set_color(10, 0); // green for usage
        print("Usage: echo ");
        graphics_console_set_color(13, 0); // magenta for args
        print("<text>");
        graphics_console_set_color(15, 0); // white for description
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
            print("Wrote file: "); print(fname); print("\n");
        } else {
            print("File write error.\n");
        }
    } else {
        print(args);
        print("\n");
    }
} 