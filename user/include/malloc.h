#pragma once
#include <stdlib.h>
size_t malloc_usable_size(void *p);      /* bytes the block really holds (>= what was asked for) */
