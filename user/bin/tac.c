/* tac [file...]: lines in reverse order */
#include <vutil.h>

int main(int argc, char **argv) {
    int rc = 0;
    for (int k = argc > 1 ? 1 : 0; k < (argc > 1 ? argc : 1); k++) {
        size_t len;
        char *b = slurp("tac", argc > 1 ? argv[k] : "-", &len);
        if (!b) { rc = 1; continue; }
        size_t end = len;
        if (!len) continue;
        int noeol = b[len - 1] != '\n';
        while (end) {
            size_t st = end - (!noeol && b[end - 1] == '\n' ? 1 : 0);
            while (st && b[st - 1] != '\n') st--;
            fwrite(b + st, 1, end - st, stdout);
            noeol = 0;
            end = st;
        }
        free(b);
    }
    return rc;
}
