#pragma once
#include "abi.h"
/* ponytail: ABI doesn't say how the exit status is encoded; assume plain exit code */
#define WIFEXITED(s) 1
#define WEXITSTATUS(s) ((s) & 0xff)
