#include "pwd.h"
#include "../drivers/console.h"
#include "../fs/filesystem.h"
#include "../drivers/graphics.h"
#include "command_utils.h"

void cmd_pwd(const char* args, const char* flag) {
    (void)args;
    if (flag && (!kstrcmp(flag, "--help") || !kstrcmp(flag, "-h"))) {
        graphics_console_set_color(rgb16(0,255,128), rgb16(0,0,0)); // bright green for usage
        print("Usage: pwd\n");
        graphics_console_set_color(rgb16(220,220,220), rgb16(0,0,0)); // soft white for description
        print("Shows the current working directory.\n");
        return;
    }
    fs_pwd();
} 