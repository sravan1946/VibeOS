/* dmesg : the kernel log ring (SYS_klog) */
#include <vutil.h>
#include <syscall.h>

int main(void) {
    enum { CAP = 1 << 18 };
    char *b = malloc(CAP);
    if (!b) { fprintf(stderr, "dmesg: out of memory\n"); return 1; }
    int n = klog(b, CAP);
    if (n < 0) { WARN("dmesg", "klog"); return 1; }
    write_all(1, b, n);
    return 0;
}
