/* hello.c - a tiny VibeOS program */
#include <stdio.h>

int main(int argc, char **argv) {
    const char *who = argc > 1 ? argv[1] : "world";
    for (int i = 0; i < 3; i++)
        printf("hello, %s! (%d)\n", who, i + 1);
    return 0;
}
