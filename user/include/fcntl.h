#pragma once
#include "abi.h"
int open(const char *path, int flags, ...);
int fcntl(int fd, int cmd, ...);
