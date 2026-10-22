/* config.h - tcc.h does #include "config.h"; this is how TinyCC is set up for VibeOS without patching it.
 * i386 target, static output, everything lives under /usr. */
#pragma once
#define TCC_VERSION "0.9.27"
#define TCC_TARGET_I386 1
#define CONFIG_TCC_STATIC 1             /* no dlopen */
#define CONFIG_TCCBOOT 1                /* drops the backtrace/bcheck machinery (needs ucontext + signals) */
#define CONFIG_TCCDIR "/usr/lib/tcc"    /* libtcc1.a + include/ (stdarg.h, stddef.h, ...) */
#define CONFIG_TCC_SYSINCLUDEPATHS "/usr/lib/tcc/include:/usr/include"
#define CONFIG_TCC_LIBPATHS "/usr/lib/tcc:/usr/lib"
#define CONFIG_TCC_CRTPREFIX "/usr/lib"
#define CONFIG_TCC_ELFINTERP ""         /* static only */
