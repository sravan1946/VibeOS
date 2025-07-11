#include "touch.h"
#include "../drivers/console.h"
#include "../fs/filesystem.h"
#include "command_utils.h"

void cmd_touch(const char* args, const char* flag) {
    if (flag && (!kstrcmp(flag, "--help") || !kstrcmp(flag, "-h"))) {
        print("Usage: touch [--help] <file>\nCreates an empty file or updates its timestamp (not implemented).\n");
        return;
    }
    const char* fname = args;
    while (*fname == ' ' || *fname == '\t') fname++;
    if (!*fname) { print("No file specified.\n"); return; }
    if (fs_write(fname, "", 0)) {
        print("Touched file: "); print(fname); print("\n");
    } else {
        print("Failed to touch file.\n");
    }
} 