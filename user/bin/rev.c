/* rev [file...]: reverse each line's characters (bytes) */
#include <vutil.h>

int main(int argc, char **argv) {
    int rc = 0;
    for (int k = argc > 1 ? 1 : 0; k < (argc > 1 ? argc : 1); k++) {
        size_t len;
        char *b = slurp("rev", argc > 1 ? argv[k] : "-", &len);
        if (!b) { rc = 1; continue; }
        for (size_t pos = 0; pos < len;) {
            char *e = memchr(b + pos, '\n', len - pos);
            size_t ll = e ? (size_t)(e - (b + pos)) : len - pos;
            for (size_t i = 0; i < ll / 2; i++) { char t = b[pos + i]; b[pos + i] = b[pos + ll - 1 - i]; b[pos + ll - 1 - i] = t; }
            fwrite(b + pos, 1, ll, stdout);
            if (e) putchar('\n');
            pos += ll + 1;
        }
        free(b);
    }
    return rc;
}
