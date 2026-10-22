#include <ctype.h>
#include "words.h"

int next_word(FILE *f, char *buf, int size) {
    int c, n = 0;
    while ((c = fgetc(f)) != EOF && !isalpha(c)) ;
    for (; c != EOF && (isalpha(c) || c == '\''); c = fgetc(f))
        if (n < size - 1) buf[n++] = tolower(c);
    buf[n] = 0;
    return n > 0;
}
