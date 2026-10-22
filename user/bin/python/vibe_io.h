/* vibe_io.h - poll() for the modules that wait on fds (gui, socket, select): ^C runs between EINTR retries */
#pragma once
#include <errno.h>
#include <poll.h>
#include "py/mphal.h"
#include "py/runtime.h"

/* timeout_ms < 0 blocks; -1 + errno on failure */
static inline int vpoll(struct pollfd *p, int n, int ms) {
    mp_uint_t end = mp_hal_ticks_ms() + (ms > 0 ? ms : 0);
    for (;;) {
        int left = ms > 0 ? (int)(end - mp_hal_ticks_ms()) : ms;
        int r = poll(p, n, ms > 0 && left < 0 ? 0 : left);
        if (r >= 0 || errno != EINTR) return r;
        mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS);
    }
}
