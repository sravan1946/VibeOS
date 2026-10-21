/* uptime : " 12:34:56 up 1 day, 2:03, 5 processes" */
#include <vutil.h>
#include <syscall.h>
#include <time.h>

int main(void) {
    uint32_t s = uptime_ms() / 1000;
    struct sysinfo si;
    sysinfo(&si);
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char t[16];
    strftime(t, sizeof t, "%H:%M:%S", &tm);
    printf(" %s up ", t);
    if (s >= 86400) printf("%u day%s, ", s / 86400, s / 86400 == 1 ? "" : "s");
    if (s % 86400 >= 3600) printf("%u:%02u", s % 86400 / 3600, s % 3600 / 60);
    else printf("%u min", s % 3600 / 60);
    printf(", %u process%s\n", si.nprocs, si.nprocs == 1 ? "" : "es");
    return 0;
}
