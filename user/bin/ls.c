/* ls [-l] [-a] [path...]   dirs blue bold, executables green, colour only on a tty */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>

static int opt_l, opt_a, color;

struct ent { char *name; struct stat st; };

static int by_name(const void *a, const void *b) {
    return strcmp(((const struct ent *)a)->name, ((const struct ent *)b)->name);
}

static const char *col(const struct stat *st) {
    if (!color) return "";
    if (S_ISDIR(st->st_mode)) return "\x1b[1;34m";
    if (S_ISCHR(st->st_mode)) return "\x1b[33m";
    if (st->st_mode & 0111) return "\x1b[32m";
    return "";
}

static void print_name(const struct ent *e) {
    printf("%s%s%s", col(&e->st), e->name, color ? "\x1b[0m" : "");
}

static void print_long(const struct ent *e) {
    char t[24] = "";
    time_t mt = e->st.st_mtime;
    struct tm tm;
    localtime_r(&mt, &tm);
    strftime(t, sizeof t, "%b %e %H:%M", &tm);
    unsigned m = e->st.st_mode;
    char type = S_ISDIR(m) ? 'd' : S_ISCHR(m) ? 'c' : S_ISFIFO(m) ? 'p' : '-';
    printf("%c%c%c%c%c%c%c%c%c%c %2u %8u %s ", type,
           m & 0400 ? 'r' : '-', m & 0200 ? 'w' : '-', m & 0100 ? 'x' : '-',
           m & 040 ? 'r' : '-', m & 020 ? 'w' : '-', m & 010 ? 'x' : '-',
           m & 04 ? 'r' : '-', m & 02 ? 'w' : '-', m & 01 ? 'x' : '-',
           (unsigned)e->st.st_nlink, (unsigned)e->st.st_size, t);
    print_name(e);
    putchar('\n');
}

static int list_dir(const char *path) {
    DIR *d = opendir(path);
    if (!d) { fprintf(stderr, "ls: %s: %s\n", path, strerror(errno)); return 1; }
    struct ent *v = NULL;
    size_t n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!opt_a && de->d_name[0] == '.') continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            v = realloc(v, cap * sizeof *v);
            if (!v) { fprintf(stderr, "ls: out of memory\n"); return 1; }
        }
        char full[PATH_MAX];
        snprintf(full, sizeof full, "%s/%s", path, de->d_name);
        v[n].name = strdup(de->d_name);
        if (stat(full, &v[n].st) < 0) memset(&v[n].st, 0, sizeof v[n].st);
        n++;
    }
    closedir(d);
    qsort(v, n, sizeof *v, by_name);
    for (size_t i = 0; i < n; i++) {
        if (opt_l) print_long(&v[i]);
        else { print_name(&v[i]); fputs(i + 1 < n ? "  " : "\n", stdout); }
        free(v[i].name);
    }
    free(v);
    return 0;
}

int main(int argc, char **argv) {
    int i = 1, rc = 0, npaths;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (const char *f = argv[i] + 1; *f; f++) {
            if (*f == 'l') opt_l = 1;
            else if (*f == 'a') opt_a = 1;
            else { fprintf(stderr, "usage: ls [-la] [path...]\n"); return 2; }
        }
    color = isatty(1);
    npaths = argc - i;
    if (!npaths) return list_dir(".");
    for (; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) < 0) { fprintf(stderr, "ls: %s: %s\n", argv[i], strerror(errno)); rc = 1; continue; }
        if (S_ISDIR(st.st_mode)) {
            if (npaths > 1) printf("%s:\n", argv[i]);
            rc |= list_dir(argv[i]);
        } else {
            struct ent e = { argv[i], st };
            if (opt_l) print_long(&e); else { print_name(&e); putchar('\n'); }
        }
    }
    return rc;
}
