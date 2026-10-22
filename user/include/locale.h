#pragma once
#define LC_ALL 6
#define LC_COLLATE 3
#define LC_CTYPE 0
#define LC_MONETARY 4
#define LC_NUMERIC 1
#define LC_TIME 2
struct lconv { char *decimal_point, *thousands_sep, *grouping; };
char *setlocale(int category, const char *locale);   /* only "C" / "" / "POSIX" succeed */
struct lconv *localeconv(void);
