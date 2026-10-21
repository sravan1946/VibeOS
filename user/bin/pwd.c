/* pwd */
#include <vutil.h>

int main(void) {
    char b[PATH_MAX];
    if (!getcwd(b, sizeof b)) { WARN("pwd", "getcwd"); return 1; }
    puts(b);
    return 0;
}
