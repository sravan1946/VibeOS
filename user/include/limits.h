/* limits.h - gcc's version chains to a hosted libc header; keep it self-contained */
#pragma once
#include "abi.h"
#define CHAR_BIT 8
#define SCHAR_MIN (-128)
#define SCHAR_MAX 127
#define UCHAR_MAX 255
#define CHAR_MIN SCHAR_MIN
#define CHAR_MAX SCHAR_MAX
#define SHRT_MIN (-32768)
#define SHRT_MAX 32767
#define USHRT_MAX 65535
#define INT_MAX 2147483647
#define INT_MIN (-INT_MAX - 1)
#define UINT_MAX 4294967295u
#define LONG_MAX 2147483647L
#define LONG_MIN (-LONG_MAX - 1L)
#define ULONG_MAX 4294967295UL
#define LLONG_MAX 9223372036854775807LL
#define LLONG_MIN (-LLONG_MAX - 1LL)
#define ULLONG_MAX 18446744073709551615ULL
#define SSIZE_MAX INT_MAX
