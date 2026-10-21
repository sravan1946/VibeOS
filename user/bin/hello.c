/* hello - prints argv and environment */
#include <stdio.h>

int main(int argc, char **argv, char **envp) {
    printf("Hello from VibeOS userland!\n");
    for (int i = 0; i < argc; i++) printf("argv[%d] = \"%s\"\n", i, argv[i]);
    for (char **e = envp; e && *e; e++) printf("env: %s\n", *e);
    return 0;
}
