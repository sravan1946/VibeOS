#include "ls.h"
#include "../drivers/console.h"
#include "../fs/filesystem.h"
#include "command_utils.h"

void cmd_ls(const char* args, const char* flag) {
    if (flag && (!kstrcmp(flag, "--help") || !kstrcmp(flag, "-h"))) {
        print("Usage: ls [--help] [dir]\nLists all files and directories in the given directory (default: current).\n");
        return;
    }
    while (*args == ' ' || *args == '\t') args++;
    fs_list(args);
} 