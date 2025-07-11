#ifndef FILESYSTEM_H
#define FILESYSTEM_H

#define MAX_FILES 16
#define MAX_FILENAME 16
#define MAX_FILESIZE 256

typedef struct {
    char name[MAX_FILENAME];
    int size;
    char data[MAX_FILESIZE];
} file_entry;

extern file_entry files[MAX_FILES];
extern int file_count;

file_entry* find_file(const char* name);
file_entry* create_file(const char* name);
int write_file(const char* name, const char* data, int len);
const char* read_file(const char* name, int* out_len);
void list_files(void);

#endif // FILESYSTEM_H 