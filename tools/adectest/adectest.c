/* adectest FILE [SEEK_MS] : decode with user/music/adec.c and write 48 kHz stereo s16le to stdout; the file's
 * length, info and tags go to stderr as "key=value" lines. Driven by test.py. */
#include <stdio.h>
#include <stdlib.h>
#include "adec.h"

int main(int argc, char **argv) {
    const char *err = "";
    struct adec_tags t;
    struct adec *d = argc > 1 ? adec_open(argv[1], &t, &err) : NULL;
    if (!d) { fprintf(stderr, "error=%s\n", err); return 1; }
    fprintf(stderr, "len_ms=%d\ninfo=%s\ntitle=%s\nartist=%s\nalbum=%s\nart=%zu\n", adec_length_ms(d), adec_info(d), t.title, t.artist, t.album, t.art_len);
    if (argc > 2 && adec_seek(d, atoi(argv[2])) < 0) { fprintf(stderr, "error=seek\n"); return 1; }
    static int16_t buf[1000 * 2];                   /* odd size: the chunking must not matter */
    int n;
    while ((n = adec_read(d, buf, 1000)) > 0) fwrite(buf, 4, n, stdout);
    adec_close(d);
    return 0;
}
