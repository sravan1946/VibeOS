#pragma once
#include <stddef.h>
#include <stdint.h>
typedef int pid_t;
typedef int32_t off_t;
typedef int ssize_t;
typedef uint32_t mode_t;
typedef long time_t;      /* ponytail: signed 32-bit, 2038; widen with SYS_time */
typedef uint32_t ino_t;
