/* shim.h - host build of kernel/fs/ext2.c: libc first, kernel.h's decls are compatible;
 * kmalloc & co, panic, ticks_ms live in test.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
