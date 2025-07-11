#include "filesystem.h"
#include "console.h"

// Minimal kernel string compare
int kstrcmp(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a - *b;
}
int kstrncmp(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i] || !a[i] || !b[i]) return a[i] - b[i];
    }
    return 0;
}

file_entry files[MAX_FILES];
int file_count = 0;

file_entry* find_file(const char* name) {
    for (int i = 0; i < file_count; i++) {
        if (!kstrcmp(files[i].name, name)) return &files[i];
    }
    return 0;
}

file_entry* create_file(const char* name) {
    if (file_count >= MAX_FILES) return 0;
    file_entry* f = &files[file_count++];
    for (int i = 0; i < MAX_FILENAME; i++) f->name[i] = 0;
    for (int i = 0; i < MAX_FILESIZE; i++) f->data[i] = 0;
    for (int i = 0; i < MAX_FILENAME-1 && name[i]; i++) f->name[i] = name[i];
    f->size = 0;
    return f;
}

int write_file(const char* name, const char* data, int len) {
    if (len > MAX_FILESIZE) len = MAX_FILESIZE;
    file_entry* f = find_file(name);
    if (!f) f = create_file(name);
    if (!f) return 0;
    for (int i = 0; i < len; i++) f->data[i] = data[i];
    f->size = len;
    return 1;
}

const char* read_file(const char* name, int* out_len) {
    file_entry* f = find_file(name);
    if (!f) return 0;
    if (out_len) *out_len = f->size;
    return f->data;
}

void list_files(void) {
    if (file_count == 0) {
        print("No files found.\n");
        return;
    }
    for (int i = 0; i < file_count; i++) {
        print(files[i].name);
        print("\n");
    }
} 