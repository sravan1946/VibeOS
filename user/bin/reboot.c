/* reboot */
#include <vutil.h>
#include <syscall.h>

int main(void) {
    puts("Rebooting...");
    fflush(stdout);
    shutdown(1);
    return 1;
}
