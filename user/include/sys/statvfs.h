#pragma once
#include "abi.h"
#include <sys/types.h>
typedef uint32_t fsblkcnt_t;
typedef uint32_t fsfilcnt_t;
struct statvfs {
    unsigned long f_bsize, f_frsize;               /* f_frsize == f_bsize here */
    fsblkcnt_t f_blocks, f_bfree, f_bavail;        /* in f_frsize units; bavail excludes ext2's root reserve */
    fsfilcnt_t f_files, f_ffree, f_favail;
    unsigned long f_fsid, f_flag, f_namemax;       /* f_flag: ST_RDONLY (abi.h) */
};
int statvfs(const char *path, struct statvfs *sv);
int fstatvfs(int fd, struct statvfs *sv);
