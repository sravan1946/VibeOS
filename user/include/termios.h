#pragma once
#include "abi.h"
#define TCSANOW 0
int tcgetattr(int fd, struct termios *t);
int tcsetattr(int fd, int when, const struct termios *t);
void cfmakeraw(struct termios *t);
