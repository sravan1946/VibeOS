/* vibe.c - /bin/tcc entry: tcc's own main (renamed tcc_main) with VibeOS defaults put in front of the user's
 * options: static output linked at the user load address (see user/user.ld), so a plain `tcc a.c -o a` works.
 *
 * `tcc ... -run file.c args`: tcc would relocate the program into its own memory, together with a private copy of
 * libc (libc.a is linked in memory). That copy is never initialised or exited: its stdout buffer is never flushed
 * (`tcc -run x.c | cat` printed nothing), environ is empty, atexit handlers never run. So -run compiles to a
 * temporary executable and spawns it instead, which is what the user asked for anyway, minus the file. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>

int tcc_main(int argc, char **argv);

static void ignore(int sig) { (void)sig; }       /* a handler (not SIG_IGN, which the child would inherit) so ^C only ends the child */

int main(int argc, char **argv) {
    char **v = malloc((argc + 6) * sizeof *v);
    int n = 0, run = 0, src = 0;
    for (int i = 1; i < argc && !run; i++) if (!strcmp(argv[i], "-run")) run = i;
    if (run) for (int i = run + 1; i < argc && !src; i++) if (argv[i][0] != '-') src = i;
    v[n++] = argv[0];
    v[n++] = "-static";
    v[n++] = "-Wl,-Ttext=0x40001000";
    if (!src) {                                   /* plain compile, or -run with nothing to run (tcc reports it) */
        for (int i = 1; i < argc; i++) v[n++] = argv[i];
        v[n] = NULL;
        return tcc_main(n, v);
    }
    char tmp[64];
    snprintf(tmp, sizeof tmp, "/tmp/tccrun.%d", (int)getpid());
    for (int i = 1; i <= src; i++) if (i != run) v[n++] = argv[i];
    v[n++] = "-o";
    v[n++] = tmp;
    v[n] = NULL;
    int rc = tcc_main(n, v);
    if (rc) { unlink(tmp); return rc; }
    char **pv = malloc((argc - src + 1) * sizeof *pv);
    int m = 0;
    pv[m++] = argv[src];
    for (int i = src + 1; i < argc; i++) pv[m++] = argv[i];
    pv[m] = NULL;
    fflush(NULL);
    signal(SIGINT, ignore);
    int pid = spawnve(tmp, pv, environ, NULL), st = 0;
    if (pid < 0) { fprintf(stderr, "tcc: cannot run %s: %s\n", argv[src], strerror(errno)); unlink(tmp); return 1; }
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) ;
    unlink(tmp);
    return st;
}
