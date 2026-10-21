/* console.h - framebuffer + boot text console */
#pragma once
#include "kernel.h"
#include "gfx.h"

struct regs;
extern struct canvas fb;          /* the real screen */
void fb_init(void);
void bootcon_write(const char *s);
void bootcon_enable(bool on);     /* the desktop turns it off */
void splash_progress(int pct);    /* boot splash bar, 0..100 */
NORETURN void bootcon_panic(const char *msg, struct regs *r);
