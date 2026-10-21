/* whoami : single-user OS */
#include <vutil.h>

int main(void) {
    const char *u = getenv("USER");
    puts(u && *u ? u : "root");
    return 0;
}
