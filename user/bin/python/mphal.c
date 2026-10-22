/* mphal.c - MicroPython's HAL on VibeOS syscalls: tty I/O, raw mode, clocks, ^C, randomness */
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <termios.h>
#include <sys/time.h>
#include <syscall.h>

#include "py/mphal.h"
#include "py/runtime.h"

static struct termios orig;
static int have_orig;

void mp_hal_stdio_mode_raw(void) {
    struct termios raw;
    if (tcgetattr(0, &orig) < 0) return;          /* not a tty: read it as it comes */
    have_orig = 1;
    raw = orig;
    cfmakeraw(&raw);
    tcsetattr(0, TCSANOW, &raw);
}

void mp_hal_stdio_mode_orig(void) {
    if (have_orig) tcsetattr(0, TCSANOW, &orig);
}

static void sigint(int sig) {
    mp_sched_keyboard_interrupt();
}

/* ^C arrives as SIGINT while code runs (cooked tty); the REPL's own reads see it as byte 3 */
void mp_hal_set_interrupt_char(char c) {
    signal(SIGINT, c == CHAR_CTRL_C ? sigint : SIG_DFL);
}

int mp_hal_stdin_rx_chr(void) {
    unsigned char c;
    ssize_t n;
    while ((n = read(0, &c, 1)) < 0 && errno == EINTR) mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS);
    if (n <= 0) return CHAR_CTRL_D;               /* EOF ends the REPL */
    return c;
}

mp_uint_t mp_hal_stdout_tx_strn(const char *s, size_t len) {
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(1, s + done, len - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        done += n;
    }
    return done;
}

void mp_hal_stdout_tx_str(const char *s) {
    mp_hal_stdout_tx_strn(s, strlen(s));
}

void mp_hal_stdout_tx_strn_cooked(const char *s, size_t len) {
    mp_hal_stdout_tx_strn(s, len);                /* the tty does the \n -> \r\n */
}

mp_uint_t mp_hal_ticks_ms(void) { return uptime_ms(); }
mp_uint_t mp_hal_ticks_us(void) { return uptime_ms() * 1000; }   /* ponytail: ms clock; a TSC would give real us */

uint64_t mp_hal_time_ns(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000000ull + (uint64_t)tv.tv_usec * 1000;
}

void mp_hal_delay_ms(mp_uint_t ms) {
    if (ms) __syscall1(SYS_sleep, ms);
}

void mp_hal_delay_us(mp_uint_t us) {
    mp_hal_delay_ms((us + 999) / 1000);
}

void mp_hal_get_random(size_t n, uint8_t *buf) {
    int fd = open("/dev/urandom", O_RDONLY);
    size_t got = 0;
    if (fd >= 0) {
        ssize_t r;
        while (got < n && (r = read(fd, buf + got, n - got)) > 0) got += r;
        close(fd);
    }
    for (uint32_t x = uptime_ms() * 2654435761u; got < n; got++) buf[got] = x = x * 1103515245u + 12345u;
}

unsigned long mp_random_seed_init(void) {
    unsigned long r;
    mp_hal_get_random(sizeof r, (uint8_t *)&r);
    return r;
}
