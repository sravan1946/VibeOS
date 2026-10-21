#pragma once
#include "abi.h"     /* SIGINT SIGKILL SIGTERM */
#include <sys/types.h>
int kill(pid_t pid, int sig);
int raise(int sig);
