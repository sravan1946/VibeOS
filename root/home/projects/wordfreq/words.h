#ifndef WORDS_H
#define WORDS_H
#include <stdio.h>
/* read the next lowercase word (letters and apostrophes) into buf; 0 at end of file */
int next_word(FILE *f, char *buf, int size);
#endif
