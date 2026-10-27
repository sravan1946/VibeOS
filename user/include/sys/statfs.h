#pragma once
#include "abi.h"
#include <sys/types.h>
/* struct statfs and the *_MAGIC constants are in abi.h */
int statfs(const char *path, struct statfs *sf);
int fstatfs(int fd, struct statfs *sf);
