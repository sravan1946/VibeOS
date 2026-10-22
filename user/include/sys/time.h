#pragma once
#include <sys/types.h>
#include <time.h>
struct timeval { time_t tv_sec; long tv_usec; };
int gettimeofday(struct timeval *tv, void *tz);     /* ms resolution: RTC seconds + uptime ms */
