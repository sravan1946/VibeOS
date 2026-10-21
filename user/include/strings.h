#pragma once
#include <string.h>
#define bzero(p, n) memset((p), 0, (n))
#define bcopy(s, d, n) memmove((d), (s), (n))
#define index strchr
#define rindex strrchr
