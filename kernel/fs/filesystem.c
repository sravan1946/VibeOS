#include "filesystem.h"
#include "../drivers/console.h"

fs_node fs_nodes_pool[MAX_FILES];
int fs_nodes_used = 0;
fs_node* fs_root = 0;
fs_node* current_directory = 0;

static int kstrcmp(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a - *b;
}
static int kstrncmp(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i] || !a[i] || !b[i]) return a[i] - b[i];
    }
    return 0;
}
static int kstrlen(const char* s) { int i = 0; while (s[i]) i++; return i; }

static fs_node* alloc_node(void) {
    if (fs_nodes_used >= MAX_FILES) return 0;
    fs_node* n = &fs_nodes_pool[fs_nodes_used++];
    for (int i = 0; i < MAX_FILENAME; i++) n->name[i] = 0;
    n->type = FS_FILE;
    n->size = 0;
    for (int i = 0; i < MAX_FILESIZE; i++) n->data[i] = 0;
    n->parent = 0;
    for (int i = 0; i < MAX_CHILDREN; i++) n->children[i] = 0;
    n->child_count = 0;
    return n;
}

static void split_path(const char* path, char* out_parent, char* out_name) {
    int len = kstrlen(path);
    int last_slash = -1;
    for (int i = 0; i < len; i++) if (path[i] == '/') last_slash = i;
    if (last_slash < 0) {
        out_parent[0] = 0;
        for (int i = 0; i < len; i++) out_name[i] = path[i];
        out_name[len] = 0;
    } else {
        for (int i = 0; i < last_slash; i++) out_parent[i] = path[i];
        out_parent[last_slash] = 0;
        int j = 0;
        for (int i = last_slash+1; i < len; i++) out_name[j++] = path[i];
        out_name[j] = 0;
    }
}

void add_dot_entries(fs_node* dir) {
    // Add '.'
    if (dir->child_count < MAX_CHILDREN) {
        fs_node* dot = dir;
        // Do not set dot->name
        dir->children[dir->child_count++] = dot;
    }
    // Add '..'
    if (dir->child_count < MAX_CHILDREN) {
        fs_node* dotdot = dir->parent ? dir->parent : dir;
        // Do not set dotdot->name
        dir->children[dir->child_count++] = dotdot;
    }
}

static fs_node* resolve_path(const char* path, int stop_at_parent, fs_node** out_parent) {
    fs_node* node = (path[0] == '/') ? fs_root : current_directory;
    int i = (path[0] == '/') ? 1 : 0;
    char part[MAX_FILENAME];
    int part_len = 0;
    fs_node* parent = 0;
    while (path[i]) {
        if (path[i] == '/') {
            part[part_len] = 0;
            if (part_len > 0) {
                if (!kstrcmp(part, ".")) {
                    // Stay at current node
                } else if (!kstrcmp(part, "..")) {
                    if (node->parent) node = node->parent;
                } else {
                    int found = 0;
                    for (int c = 0; c < node->child_count; c++) {
                        if (!kstrcmp(node->children[c]->name, part) && node->children[c]->type == FS_DIR) {
                            parent = node;
                            node = node->children[c];
                            found = 1;
                            break;
                        }
                    }
                    if (!found) return 0;
                }
            }
            part_len = 0;
        } else {
            part[part_len++] = path[i];
        }
        i++;
    }
    part[part_len] = 0;
    if (stop_at_parent && part_len > 0) {
        if (out_parent) *out_parent = node;
        return 0;
    }
    if (part_len == 0) {
        if (out_parent) *out_parent = parent;
        return node;
    }
    if (!kstrcmp(part, ".")) {
        if (out_parent) *out_parent = node->parent;
        return node;
    } else if (!kstrcmp(part, "..")) {
        if (out_parent) *out_parent = node->parent ? node->parent->parent : node->parent;
        return node->parent ? node->parent : node;
    }
    for (int c = 0; c < node->child_count; c++) {
        if (!kstrcmp(node->children[c]->name, part)) {
            if (out_parent) *out_parent = node;
            return node->children[c];
        }
    }
    if (out_parent) *out_parent = node;
    return 0;
}

fs_node* fs_find(const char* path) {
    return resolve_path(path, 0, 0);
}

fs_node* fs_create(const char* path, fs_node_type type) {
    char parent_path[MAX_FILENAME*2] = {0};
    char name[MAX_FILENAME] = {0};
    split_path(path, parent_path, name);
    fs_node* parent = (parent_path[0]) ? resolve_path(parent_path, 0, 0) : current_directory;
    if (!parent || parent->type != FS_DIR) return 0;
    for (int c = 0; c < parent->child_count; c++) {
        if (!kstrcmp(parent->children[c]->name, name)) return 0;
    }
    fs_node* n = alloc_node();
    if (!n) return 0;
    for (int i = 0; i < MAX_FILENAME-1 && name[i]; i++) n->name[i] = name[i];
    n->name[MAX_FILENAME-1] = 0;
    n->type = type;
    n->parent = parent;
    if (parent->child_count < MAX_CHILDREN) {
        parent->children[parent->child_count++] = n;
        if (type == FS_DIR) {
            n->child_count = 0;
            add_dot_entries(n);
        }
        return n;
    }
    return 0;
}

int fs_write(const char* path, const char* data, int len) {
    fs_node* n = resolve_path(path, 0, 0);
    if (!n) n = fs_create(path, FS_FILE);
    if (!n || n->type != FS_FILE) return 0;
    if (len > MAX_FILESIZE) len = MAX_FILESIZE;
    for (int i = 0; i < len; i++) n->data[i] = data[i];
    n->size = len;
    return 1;
}

const char* fs_read(const char* path, int* out_len) {
    fs_node* n = resolve_path(path, 0, 0);
    if (!n || n->type != FS_FILE) return 0;
    if (out_len) *out_len = n->size;
    return n->data;
}

void fs_list(const char* path) {
    fs_node* dir = (path && path[0]) ? resolve_path(path, 0, 0) : current_directory;
    if (!dir || dir->type != FS_DIR) {
        print("Not a directory.\n");
        return;
    }
    if (dir->child_count == 0) {
        print("No files found.\n");
        return;
    }
    for (int i = 0; i < dir->child_count; i++) {
        if (i == 0) {
            print(".");
        } else if (i == 1) {
            print("..");
        } else {
            print(dir->children[i]->name);
        }
        if (dir->children[i]->type == FS_DIR) print("/");
        print("\n");
    }
}

int fs_remove(const char* path) {
    fs_node* parent = 0;
    fs_node* n = resolve_path(path, 0, &parent);
    if (!n || !parent) return 0;
    if (n->type == FS_DIR && n->child_count > 0) return 0;
    int idx = -1;
    for (int i = 0; i < parent->child_count; i++) {
        if (parent->children[i] == n) { idx = i; break; }
    }
    if (idx < 0) return 0;
    for (int i = idx; i < parent->child_count-1; i++)
        parent->children[i] = parent->children[i+1];
    parent->child_count--;
    return 1;
}

int fs_mkdir(const char* path) {
    return fs_create(path, FS_DIR) ? 1 : 0;
}

int fs_chdir(const char* path) {
    fs_node* n = resolve_path(path, 0, 0);
    if (!n || n->type != FS_DIR) return 0;
    current_directory = n;
    return 1;
}

void fs_pwd(void) {
    char buf[MAX_FILENAME*MAX_FILES];
    int pos = MAX_FILENAME*MAX_FILES-1;
    buf[pos] = 0;
    fs_node* n = current_directory;
    while (n && n != fs_root) {
        int len = kstrlen(n->name);
        pos -= len;
        for (int i = 0; i < len; i++) buf[pos+i] = n->name[i];
        pos--;
        buf[pos+1] = '/';
        n = n->parent;
    }
    if (pos == MAX_FILENAME*MAX_FILES-1) { print("/\n"); return; }
    print(&buf[pos+1]); print("\n");
}

// Initialization
void fs_init(void) {
    fs_nodes_used = 0;
    fs_root = alloc_node();
    for (int i = 0; i < MAX_FILENAME; i++) fs_root->name[i] = 0;
    fs_root->name[0] = '/'; fs_root->name[1] = 0;
    fs_root->type = FS_DIR;
    fs_root->parent = 0;
    fs_root->child_count = 0;
    current_directory = fs_root;
    // Add . and ..
    add_dot_entries(fs_root);
} 