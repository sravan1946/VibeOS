/* vibe.c - /bin/tcc entry: tcc's own main (renamed tcc_main) with VibeOS defaults put in front of the user's
 * options: static output linked at the user load address (see user/user.ld), so a plain `tcc a.c -o a` works. */
#include <stdlib.h>

int tcc_main(int argc, char **argv);

int main(int argc, char **argv) {
    char **v = malloc((argc + 3) * sizeof *v);
    int n = 0;
    v[n++] = argv[0];
    v[n++] = "-static";
    v[n++] = "-Wl,-Ttext=0x40001000";
    for (int i = 1; i < argc; i++) v[n++] = argv[i];
    v[n] = NULL;
    return tcc_main(n, v);
}
