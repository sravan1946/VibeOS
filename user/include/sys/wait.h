#pragma once
#include "abi.h"
/* kernel stores the plain exit code in *status */
#define WIFEXITED(s) 1
#define WEXITSTATUS(s) ((s) & 0xff)
