/* shutdown [-r] [now] : power off (or -r: reboot) */
#include <vutil.h>
#include <syscall.h>

int main(int argc, char **argv) {
    int reboot = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-r")) reboot = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "-P") || !strcmp(argv[i], "now")) continue;
        else { fprintf(stderr, "usage: shutdown [-r] [now]\n"); return 1; }
    }
    printf("%s...\n", reboot ? "Rebooting" : "Powering off");
    fflush(stdout);
    shutdown(reboot);
    return 1;
}
