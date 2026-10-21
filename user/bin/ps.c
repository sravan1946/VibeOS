/* ps : every task from SYS_proclist */
#include <vutil.h>
#include <syscall.h>

static const char *const st[] = { "run", "ready", "sleep", "zombie" };

int main(void) {
    struct procinfo p[128];
    int n = proclist(p, 128);
    if (n < 0) { WARN("ps", "proclist"); return 1; }
    printf("%5s %5s %-6s %8s %9s  %s\n", "PID", "PPID", "STATE", "MEM(KB)", "CPU(ms)", "NAME");
    for (int i = 0; i < n; i++)
        printf("%5d %5d %-6s %8u %9u  %s\n", p[i].pid, p[i].ppid, p[i].state >= 0 && p[i].state <= 3 ? st[p[i].state] : "?",
               p[i].mem_kb, p[i].cpu_ms, p[i].name);
    return 0;
}
