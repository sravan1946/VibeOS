#pragma once
#include <stddef.h>
#include <sys/types.h>
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
/* user pages are always RWX (no NX on i686): this only validates and returns 0 */
int mprotect(void *addr, size_t len, int prot);
