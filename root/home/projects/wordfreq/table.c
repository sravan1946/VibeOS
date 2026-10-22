#include <stdlib.h>
#include <string.h>
#include "table.h"

static struct entry *tab;
static int n, cap;

void table_add(const char *word) {
    for (int i = 0; i < n; i++)
        if (!strcmp(tab[i].word, word)) { tab[i].count++; return; }
    if (n == cap) tab = realloc(tab, (cap = cap ? cap * 2 : 64) * sizeof *tab);
    tab[n].word = strdup(word);
    tab[n++].count = 1;
}

static int by_count(const void *a, const void *b) {
    const struct entry *x = a, *y = b;
    return y->count != x->count ? y->count - x->count : strcmp(x->word, y->word);
}

struct entry *table_sorted(int *count) {
    qsort(tab, n, sizeof *tab, by_count);
    *count = n;
    return tab;
}
