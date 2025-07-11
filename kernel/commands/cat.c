#include "cat.h"
#include "../drivers/console.h"
#include "../fs/filesystem.h"
#include "command_utils.h"

void cmd_cat(const char* args, const char* flag) {
    if (flag && (!kstrcmp(flag, "--help") || !kstrcmp(flag, "-h"))) {
        print("Usage: cat [--help] <file>\nShows the contents of a file.\n");
        return;
    }
    const char* fname = args;
    while (*fname == ' ' || *fname == '\t') fname++;
    int len = 0;
    const char* data = read_file(fname, &len);
    if (!data) {
        print("File not found.\n");
    } else {
        for (int i = 0; i < len; i++) print_char(data[i]);
        print("\n");
    }
} 