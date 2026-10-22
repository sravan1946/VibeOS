/* wordfreq [file...]: count words, most frequent first */
#include <stdio.h>
#include "table.h"
#include "words.h"

int main(int argc, char **argv) {
    char w[64];
    for (int i = 1; i < argc || i == 1; i++) {
        FILE *f = argc > 1 ? fopen(argv[i], "r") : stdin;
        if (!f) { fprintf(stderr, "wordfreq: cannot open %s\n", argv[i]); return 1; }
        while (next_word(f, w, sizeof w)) table_add(w);
        if (f != stdin) fclose(f);
    }
    int n;
    struct entry *e = table_sorted(&n);
    for (int i = 0; i < n; i++) printf("%d %s\n", e[i].count, e[i].word);
    return 0;
}
