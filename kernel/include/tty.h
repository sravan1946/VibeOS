/* tty.h - terminals: line discipline between keyboard and a terminal emulator */
#pragma once
#include "vfs.h"

#define TTY_BUF 4096
#define TTY_LINE 1024

struct tty {
    struct vnode vn;                /* the device node processes open */
    char in[TTY_BUF];               /* bytes ready for read() */
    uint32_t in_head, in_tail;
    char line[TTY_LINE];            /* canonical-mode edit buffer */
    int line_len;
    int eof;                        /* ^D pending */
    uint32_t lflag;                 /* ECHO | ICANON | ISIG */
    int fg_pid;
    struct winsize ws;
    void (*out)(void *ctx, const char *s, int n);
    void *ctx;
    int dead;                       /* the terminal went away */
};

struct tty *tty_create(void (*out)(void *ctx, const char *s, int n), void *ctx, int cols, int rows);
void tty_hangup(struct tty *t);                         /* terminal closed: readers get EOF, fg gets SIGKILL */
void tty_input(struct tty *t, const char *s, int n);   /* bytes typed */
void tty_key(struct tty *t, const struct gui_event *ev); /* key press -> bytes (arrows -> ESC [ A ...) */
void tty_output(struct tty *t, const char *s, int n);  /* to the screen, \n -> \r\n */
void tty_set_winsize(struct tty *t, int cols, int rows, int pw, int ph);
void tty_proc_exited(struct tty *t, int pid);

/* keyboard (dev/keyboard.c): every key event goes to this sink */
void keyboard_init(void);
extern void (*key_sink)(const struct gui_event *ev);
int  keyboard_mods(void);
