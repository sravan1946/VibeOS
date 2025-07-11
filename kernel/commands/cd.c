#include "cd.h"
#include "../drivers/console.h"
#include "../fs/filesystem.h"
#include "command_utils.h"

void cmd_cd(const char* args, const char* flag) {
    if (flag && (!kstrcmp(flag, "--help") || !kstrcmp(flag, "-h"))) {
        print("Usage: cd [--help] <dir>\nChanges the current working directory.\n");
        return;
    }
    const char* dir = args;
    while (*dir == ' ' || *dir == '\t') dir++;
    if (!*dir) { print("No directory specified.\n"); return; }
    if (fs_chdir(dir)) {
        // Success, print nothing
    } else {
        print("No such directory.\n");
    }
} 