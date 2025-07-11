#include "rm.h"
#include "../drivers/console.h"
#include "../fs/filesystem.h"
#include "command_utils.h"

void cmd_rm(const char* args, const char* flag) {
    if (flag && (!kstrcmp(flag, "--help") || !kstrcmp(flag, "-h"))) {
        print("Usage: rm [--help] <file|dir>\nRemoves a file or an empty directory.\n");
        return;
    }
    const char* path = args;
    while (*path == ' ' || *path == '\t') path++;
    if (!*path) { print("No file or directory specified.\n"); return; }
    if (fs_remove(path)) {
        print("Removed: "); print(path); print("\n");
    } else {
        print("Failed to remove (not found or not empty).\n");
    }
} 