/* dirname NAME... */
#include <vutil.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: dirname NAME...\n"); return 1; }
    for (int i = 1; i < argc; i++) {
        char *p = strdup(argv[i]);
        size_t l = strlen(p);
        while (l > 1 && p[l - 1] == '/') p[--l] = 0;
        char *s = strrchr(p, '/');
        if (!s) puts(".");
        else {
            while (s > p && s[-1] == '/') s--;
            if (s == p) puts("/"); else { *s = 0; puts(p); }
        }
        free(p);
    }
    return 0;
}
