#pragma once
#include "abi.h"
typedef struct DIR DIR;
DIR *opendir(const char *path);
struct dirent *readdir(DIR *d);
void rewinddir(DIR *d);
int closedir(DIR *d);
