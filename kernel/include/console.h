/* console.h - framebuffer + boot text console */
#pragma once
#include "kernel.h"
#include "gfx.h"

struct regs;
extern struct canvas fb;          /* the real screen */
void fb_init(void);
void fb_set_mode(uint32_t *px, int w, int h, int stride);   /* new mode: repoint (and redraw the splash) */
void bootcon_write(const char *s);
void bootcon_enable(bool on);     /* the desktop turns it off */
void splash_progress(int pct);    /* boot splash bar, 0..100 */
NORETURN void bootcon_panic(const char *msg, struct regs *r);

/* dev/display.c: Bochs/QEMU DISPI mode setting, /dev/display */
void display_init(void);          /* after pci_init + root mount; applies /etc/display */
int  display_mode(int w, int h);  /* adapter + fb + pointer scale only; use wm_set_mode once the desktop runs */
