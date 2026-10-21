/* clear */
#include <vutil.h>

int main(void) {
    fputs("\x1b[H\x1b[2J", stdout);
    return 0;
}
