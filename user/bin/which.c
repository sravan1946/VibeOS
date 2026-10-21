/* which name... : first executable on $PATH (default /bin) */
#include <vutil.h>

static int exec_file(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0111);
}

int main(int argc, char **argv) {
    int rc = 0;
    if (argc < 2) { fprintf(stderr, "usage: which name...\n"); return 1; }
    const char *path = getenv("PATH");
    if (!path || !*path) path = "/bin";
    for (int i = 1; i < argc; i++) {
        char buf[PATH_MAX];
        int found = 0;
        if (strchr(argv[i], '/')) { if (exec_file(argv[i])) { puts(argv[i]); found = 1; } }
        else for (const char *p = path; *p && !found;) {
            const char *e = strchr(p, ':');
            size_t dl = e ? (size_t)(e - p) : strlen(p);
            snprintf(buf, sizeof buf, "%.*s/%s", (int)dl, dl ? p : ".", argv[i]);
            if (exec_file(buf)) { puts(buf); found = 1; }
            p = e ? e + 1 : p + dl;
        }
        if (!found) rc = 1;
    }
    return rc;
}
