/* fsutil.h - file associations (/etc/open.conf), the trash (/home/.trash) and recursive copy/delete.
 * Plain libc only (also builds on the host: tools/filestest). Used by files, desktop, open and trash. */
#pragma once
#include <stdint.h>

enum { FK_DIR, FK_EXE, FK_TEXT, FK_CODE, FK_IMAGE, FK_AUDIO, FK_HTML, FK_FILE };
int  fs_kind(const char *name, int isdir, int isexe);        /* icon class from the extension */

/* Association table: "ext,ext,... program[|fallback...]" per line, "*" matches anything; the file is the argument.
 * 1 = *prog is the program; 2 = the file is an executable and runs itself; 0 = no entry. */
int  fs_assoc_text(const char *conf, const char *path, int isexe, char *prog, int n);
int  fs_assoc(const char *path, char *prog, int n);          /* /etc/open.conf + stat() */
int  fs_open(const char *path);                              /* start the associated program, detached: pid or -1 */

/* Trash: <fs_trash_dir>/files/NAME holds the item, <fs_trash_dir>/info/NAME "original path\ndeleted epoch\n" */
extern const char *fs_trash_dir;
int  fs_trash_put(const char *path, char *name, int n);      /* 0 / -1; name = how it is stored */
int  fs_trash_info(const char *name, char *orig, int n, uint32_t *when);
int  fs_trash_restore(const char *name, char *dest, int n);  /* 0 / -1; dest = where it went */
int  fs_trash_empty(void);
int  fs_trash_count(void);
int  fs_in_trash(const char *path);

/* Cancellable work: the hook runs at the start of every entry (bytes 0, path = the entry) and after every chunk copied
 * (bytes = chunk size); a non-zero return stops with FS_CANCEL. A cancelled or failed copy removes what it wrote. */
typedef int (*fs_hook)(void *ctx, const char *path, uint32_t bytes);
#define FS_CANCEL (-2)
int  fs_copy_x(const char *src, const char *dst, fs_hook h, void *ctx);   /* dst must not exist */
int  fs_rmrf_x(const char *path, fs_hook h, void *ctx);
int  fs_trash_empty_x(fs_hook h, void *ctx);

int  fs_rmrf(const char *path);
int  fs_copy(const char *src, const char *dst);              /* file or tree; refuses a folder into itself */
int  fs_mkdirs(const char *path);
void fs_unique(char *path, int n);                           /* "a.txt" -> "a (2).txt" when it exists */
void fs_du(const char *path, uint32_t *files, uint32_t *dirs, uint64_t *bytes);   /* bounded walk */
const char *fs_base(const char *path);
