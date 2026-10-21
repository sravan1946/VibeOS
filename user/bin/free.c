/* free [-k|-m|-h] : physical memory and kernel heap */
#include <vutil.h>
#include <syscall.h>

static int unit = 1;                 /* 1 = KiB, 1024 = MiB, 0 = human */

static void cell(uint32_t kb) {
    if (unit) printf("%12u", kb / unit);
    else {
        char b[16];
        if (kb >= 1024 * 1024) snprintf(b, sizeof b, "%u.%uG", kb >> 20, (kb & 0xFFFFF) * 10 >> 20);
        else if (kb >= 1024) snprintf(b, sizeof b, "%u.%uM", kb >> 10, (kb & 1023) * 10 >> 10);
        else snprintf(b, sizeof b, "%uK", kb);
        printf("%12s", b);
    }
}

static void row(const char *name, uint32_t total, uint32_t used) {
    printf("%-6s", name);
    cell(total); cell(used); cell(total - used);
    putchar('\n');
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-m")) unit = 1024;
        else if (!strcmp(argv[i], "-k")) unit = 1;
        else if (!strcmp(argv[i], "-h")) unit = 0;
        else { fprintf(stderr, "usage: free [-k|-m|-h]\n"); return 1; }
    }
    struct sysinfo si;
    if (sysinfo(&si) < 0) { WARN("free", "sysinfo"); return 1; }
    printf("%6s%12s%12s%12s\n", "", "total", "used", "free");
    row("Mem:", si.mem_total_kb, si.mem_total_kb - si.mem_free_kb);
    row("Heap:", si.heap_total_kb, si.heap_used_kb);
    return 0;
}
