#include "ls.h"
#include "../drivers/console.h"
#include "../fs/filesystem.h"
#include "../drivers/graphics.h"
#include "command_utils.h"

void cmd_ls(const char* args, const char* flag) {
    if (flag && (!kstrcmp(flag, "--help") || !kstrcmp(flag, "-h"))) {
        graphics_console_set_color(10, 0); // green for usage
        print("Usage: ls ");
        graphics_console_set_color(13, 0); // magenta for args
        print("[dir]\n");
        graphics_console_set_color(15, 0); // white for description
        print("Lists all files and directories in the given directory (default: current).\n");
        return;
    }
    while (*args == ' ' || *args == '\t') args++;
    fs_node* dir = (args && args[0]) ? fs_find(args) : current_directory;
    if (!dir || dir->type != FS_DIR) {
        graphics_console_set_color(12, 0); // red for error
        print("Not a directory.\n");
        graphics_console_set_color(15, 0);
        return;
    }
    if (dir->child_count == 0) {
        graphics_console_set_color(11, 0); // cyan header
        print("No files found.\n");
        graphics_console_set_color(15, 0);
        return;
    }
    graphics_console_set_color(11, 0); // cyan header
    print("Files:\n");
    for (int i = 2; i < dir->child_count; i++) { // skip . and ..
        if (dir->children[i]->type == FS_DIR) {
            graphics_console_set_color(9, 0); // blue for directories
            print(dir->children[i]->name);
            print("/\n");
        } else {
            graphics_console_set_color(15, 0); // white for files
            print(dir->children[i]->name);
            print("\n");
        }
    }
    graphics_console_set_color(15, 0);
} 