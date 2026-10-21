#pragma once
typedef int jmp_buf[6];       /* ebx esi edi ebp esp eip */
int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((noreturn));
