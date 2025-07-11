#include "pwd.h"
#include "../drivers/console.h"
#include "../fs/filesystem.h"
#include "command_utils.h"

void cmd_pwd(const char* args, const char* flag) {
    (void)args;
    if (flag && (!kstrcmp(flag, "--help") || !kstrcmp(flag, "-h"))) {
        print("Usage: pwd [--help]\nPrints the current working directory.\n");
        return;
    }
    fs_pwd();
} 