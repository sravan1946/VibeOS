#ifndef FILESYSTEM_H
#define FILESYSTEM_H

#define MAX_FILES 16
#define MAX_FILENAME 16
#define MAX_FILESIZE 256
#define MAX_CHILDREN 16

typedef enum { FS_FILE, FS_DIR } fs_node_type;

typedef struct fs_node {
    char name[MAX_FILENAME];
    fs_node_type type;
    int size; // For files
    char data[MAX_FILESIZE]; // For files
    struct fs_node* parent; // For navigation
    struct fs_node* children[MAX_CHILDREN]; // For directories
    int child_count; // For directories
} fs_node;

extern fs_node* fs_root;
extern fs_node* current_directory;

fs_node* fs_find(const char* path);
fs_node* fs_create(const char* path, fs_node_type type);
int fs_write(const char* path, const char* data, int len);
const char* fs_read(const char* path, int* out_len);
void fs_list(const char* path);
int fs_remove(const char* path);
int fs_mkdir(const char* path);
int fs_chdir(const char* path);
void fs_pwd(void);
void fs_init(void);
void get_current_path(char* out, int maxlen);

// Note: Every directory contains '.' (self) and '..' (parent) as the first two children.

#endif // FILESYSTEM_H 