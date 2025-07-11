#include "mkdir.h"
#include "../drivers/console.h"
#include "../fs/filesystem.h"
#include "command_utils.h"

void cmd_mkdir(const char* args, const char* flag) {
    if (flag && (!kstrcmp(flag, "--help") || !kstrcmp(flag, "-h"))) {
        print("Usage: mkdir [--help] <dir>\nCreates a new directory.\n");
        return;
    }
    const char* dir = args;
    while (*dir == ' ' || *dir == '\t') dir++;
    if (!*dir) { print("No directory specified.\n"); return; }
    if (fs_mkdir(dir)) {
        print("Created directory: "); print(dir); print("\n");
    } else {
        print("Failed to create directory.\n");
    }
} 