/* trash FILE... | -l | -r NAME | -e : move to /home/.trash, list it, restore, or empty it */
#include <fsutil.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <limits.h>

int main(int argc, char **argv) {
    int rc = 0;
    if (argc < 2) { fprintf(stderr, "usage: trash FILE... | -l | -r NAME | -e\n"); return 2; }
    if (!strcmp(argv[1], "-e")) { if (fs_trash_empty()) { fprintf(stderr, "trash: could not empty\n"); return 1; } return 0; }
    if (!strcmp(argv[1], "-l")) {
        char d[PATH_MAX];
        snprintf(d, sizeof d, "%s/files", fs_trash_dir);
        DIR *dir = opendir(d);
        struct dirent *de;
        while (dir && (de = readdir(dir))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            char o[PATH_MAX] = "?";
            fs_trash_info(de->d_name, o, sizeof o, NULL);
            printf("%s\t%s\n", de->d_name, o);
        }
        if (dir) closedir(dir);
        return 0;
    }
    if (!strcmp(argv[1], "-r")) {
        for (int i = 2; i < argc; i++) {
            char dest[PATH_MAX];
            if (fs_trash_restore(argv[i], dest, sizeof dest)) { fprintf(stderr, "trash: cannot restore %s\n", argv[i]); rc = 1; }
            else printf("%s\n", dest);
        }
        return rc;
    }
    for (int i = 1; i < argc; i++) {
        char n[256];
        if (fs_trash_put(argv[i], n, sizeof n)) { perror(argv[i]); rc = 1; }
    }
    return rc;
}
