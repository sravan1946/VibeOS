/* yes [string...] */
#include <vutil.h>

int main(int argc, char **argv) {
    char line[1024] = "";
    for (int i = 1; i < argc; i++) { strlcat(line, argv[i], sizeof line - 2); if (i + 1 < argc) strlcat(line, " ", sizeof line - 2); }
    if (argc < 2) strcpy(line, "y");
    strcat(line, "\n");
    size_t n = strlen(line);
    for (;;) if (write_all(1, line, n) < 0) return 1;      /* ends on ^C or a closed pipe */
}
