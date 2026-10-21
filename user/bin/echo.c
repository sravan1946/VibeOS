/* echo [-n] args... */
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    int i = 1, nl = 1;
    if (i < argc && !strcmp(argv[i], "-n")) { nl = 0; i++; }
    for (; i < argc; i++) {
        fputs(argv[i], stdout);
        if (i + 1 < argc) putchar(' ');
    }
    if (nl) putchar('\n');
    return 0;
}
