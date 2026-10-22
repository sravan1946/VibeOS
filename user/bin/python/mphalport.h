/* mphalport.h - MicroPython HAL for VibeOS: stdio on fds 0/1, raw tty only while the REPL reads a line */
#pragma once
#include <errno.h>
#include <unistd.h>
#include <stdbool.h>

#ifndef CHAR_CTRL_C
#define CHAR_CTRL_C (3)
#endif

#define MICROPY_HAL_HAS_STDIO_MODE_SWITCH (1)
void mp_hal_stdio_mode_raw(void);
void mp_hal_stdio_mode_orig(void);
void mp_hal_set_interrupt_char(char c);
void mp_hal_get_random(size_t n, uint8_t *buf);
#define mp_hal_stdio_poll unused
#define mp_hal_ticks_cpu() 0

#include "py/misc.h"
#include "shared/readline/readline.h"
#define mp_hal_readline mp_hal_readline     /* input() gets the REPL's line editor */
static inline int mp_hal_readline(vstr_t *vstr, const char *p) {
    mp_hal_stdio_mode_raw();
    int ret = readline(vstr, p);
    mp_hal_stdio_mode_orig();
    return ret;
}

/* what vfs_posix uses around every syscall: retry on EINTR (after running a pending ^C), raise otherwise */
#define MP_HAL_RETRY_SYSCALL(ret, syscall, raise) { \
        for (;;) { \
            ret = syscall; \
            if (ret == -1) { \
                int err = errno; \
                if (err == EINTR) { \
                    mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS); \
                    continue; \
                } \
                raise; \
            } \
            break; \
        } \
}
