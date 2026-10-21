/* basename NAME [SUFFIX] */
#include <vutil.h>

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) { fprintf(stderr, "usage: basename NAME [SUFFIX]\n"); return 1; }
    char *p = strdup(argv[1]);
    size_t l = strlen(p);
    while (l > 1 && p[l - 1] == '/') p[--l] = 0;
    const char *b = l == 1 && *p == '/' ? p : base_name(p);
    size_t bl = strlen(b);
    if (argc == 3) {
        size_t sl = strlen(argv[2]);
        if (sl < bl && !strcmp(b + bl - sl, argv[2])) bl -= sl;
    }
    printf("%.*s\n", (int)bl, b);
    return 0;
}
