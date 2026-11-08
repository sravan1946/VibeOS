/* gz_interop: gz_interop z IN OUT (gzip_buf) / gz_interop u IN OUT (gunzip_buf); run.sh checks them against host gzip */
#include "image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 4) return 2;
    FILE *f = fopen(argv[2], "rb");
    if (!f) return 2;
    size_t cap = 1 << 20, n = 0, r;
    uint8_t *b = malloc(cap);
    while ((r = fread(b + n, 1, cap - n, f)) > 0) { n += r; if (n == cap) b = realloc(b, cap *= 2); }
    fclose(f);
    uint8_t *o; size_t on;
    if (argv[1][0] == 'z') { if (!(o = gzip_buf(b, n, &on))) return 1; }
    else if (gunzip_buf(b, n, &o, &on)) return 1;
    f = fopen(argv[3], "wb");
    fwrite(o, 1, on, f);
    free(o); free(b);
    fclose(f);
    return 0;
}
