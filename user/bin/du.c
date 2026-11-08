/* du [-ahsc] [-d N] [PATH...] : disk usage (st_blocks, 1K units; -h human, -a files too, -s summary, -c total, -d depth) */
#include <vutil.h>
#include <dirent.h>

static int all, human, summary, total_flag, maxdepth = -1, rc;

static void show(unsigned long long kb, const char *name) {
    if (human) {
        if (kb >= 1 << 20) printf("%llu.%lluG\t%s\n", kb >> 20, (kb & 0xFFFFF) * 10 >> 20, name);
        else if (kb >= 1024) printf("%llu.%lluM\t%s\n", kb >> 10, (kb & 1023) * 10 >> 10, name);
        else printf("%lluK\t%s\n", kb, name);
    } else printf("%llu\t%s\n", kb, name);
}

static unsigned long long walk(const char *path, int depth) {
    struct stat st;
    if (stat(path, &st)) { WARN("du", path); rc = 1; return 0; }
    unsigned long long kb = (st.st_blocks + 1) / 2;
    if (!S_ISDIR(st.st_mode)) {
        if (all && !summary && (maxdepth < 0 || depth <= maxdepth)) show(kb, path);
        return kb;
    }
    DIR *d = opendir(path);
    if (!d) { WARN("du", path); rc = 1; return kb; }
    char **v = NULL; int n = 0;
    for (struct dirent *e; (e = readdir(d)); ) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        v = realloc(v, ++n * sizeof *v);
        v[n - 1] = strdup(e->d_name);
    }
    closedir(d);
    for (int i = 0; i < n; i++) {
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s%s%s", path, path[strlen(path) - 1] == '/' ? "" : "/", v[i]);
        kb += walk(p, depth + 1);
        free(v[i]);
    }
    free(v);
    if (!summary && (maxdepth < 0 || depth <= maxdepth)) show(kb, path);
    return kb;
}

int main(int argc, char **argv) {
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *p = argv[i] + 1; *p; p++) {
            if (*p == 'a') all = 1;
            else if (*p == 'h') human = 1;
            else if (*p == 's') summary = 1;
            else if (*p == 'c') total_flag = 1;
            else if (*p == 'k') human = 0;
            else if (*p == 'd' && i + 1 < argc) { maxdepth = atoi(argv[++i]); break; }
            else { fputs("usage: du [-ahsck] [-d N] [PATH...]\n", stderr); return 2; }
        }
    }
    unsigned long long total = 0;
    if (i >= argc) { total = walk(".", 0); if (summary) show(total, "."); }
    else for (; i < argc; i++) { unsigned long long k = walk(argv[i], 0); total += k; if (summary) show(k, argv[i]); }
    if (total_flag) show(total, "total");
    return rc;
}
