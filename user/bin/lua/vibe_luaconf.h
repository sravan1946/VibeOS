/* vibe_luaconf.h - force-included (-include) into every Lua translation unit; this is how Lua is
 * configured for VibeOS without patching it. Not defining LUA_USE_POSIX keeps Lua on plain ISO C
 * (no popen, no dlopen, setjmp/longjmp, tmpnam), which is what our libc provides. */
#pragma once
#include <stdlib.h>
#include <unistd.h>

#define LUA_PATH_DEFAULT "/lib/lua/?.lua;/lib/lua/?/init.lua;./?.lua;./?/init.lua"
#define LUA_CPATH_DEFAULT ""            /* no C modules: no dlopen */

#ifdef VIBE_LUA_FRONTEND                /* only lua.c: our line editor and the vibe/gui modules */
char *vibe_readline(const char *prompt);
void vibe_saveline(const char *line);
#define lua_stdin_is_tty() isatty(0)
#define lua_initreadline(L) ((void)L)
#define lua_readline(L, b, p) ((void)L, ((b) = vibe_readline(p)) != NULL)
#define lua_saveline(L, line) ((void)L, vibe_saveline(line))
#define lua_freeline(L, b) ((void)L, free(b))
#define luaL_openlibs vibe_openlibs     /* defined in vibe.c: the standard libs plus vibe and gui */
#endif
