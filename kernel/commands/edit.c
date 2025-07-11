#include "edit.h"
#include "../drivers/console.h"
#include "../editor/editor.h"
#include "command_utils.h"

void cmd_edit(const char* args, const char* flag) {
    if (flag && (!kstrcmp(flag, "--help") || !kstrcmp(flag, "-h"))) {
        print("Usage: edit [--help] <file>\nOpens a file in the nano-like editor.\n");
        return;
    }
    const char* fname = args;
    while (*fname == ' ' || *fname == '\t') fname++;
    editor(fname);
} 