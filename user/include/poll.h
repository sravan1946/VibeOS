#pragma once
#include "abi.h"
typedef unsigned nfds_t;
int poll(struct pollfd *fds, nfds_t n, int timeout_ms);
