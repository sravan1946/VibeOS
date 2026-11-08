#pragma once
#include "abi.h"
/* kernel stores the plain exit code in *status; bit 8 = stopped (low byte: signal), bit 9 = continued */
#define WIFSTOPPED(s) (((s) & 0x100) != 0)
#define WSTOPSIG(s) ((s) & 0xff)
#define WIFCONTINUED(s) (((s) & 0x200) != 0)
#define WIFEXITED(s) (!((s) & 0x300))
#define WEXITSTATUS(s) ((s) & 0xff)
