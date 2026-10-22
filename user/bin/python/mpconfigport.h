/* mpconfigport.h - MicroPython configuration for /bin/python (third_party/micropython, unmodified).
 * Starts from the "extra features" level and turns off what needs hardware, threads or a native emitter. */
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#define alloca __builtin_alloca             /* no alloca.h in our libc */

#define MICROPY_CONFIG_ROM_LEVEL (MICROPY_CONFIG_ROM_LEVEL_EXTRA_FEATURES)

#define MICROPY_ENABLE_GC               (1)
#define MICROPY_ENABLE_FINALISER        (1)       /* files close when collected */
#define MICROPY_GC_SPLIT_HEAP           (1)       /* the heap grows in malloc'd chunks, see gc_get_max_new_split */
#define MICROPY_GC_SPLIT_HEAP_AUTO      (1)
#define MICROPY_STACK_CHECK             (1)
#define MICROPY_LONGINT_IMPL            (MICROPY_LONGINT_IMPL_MPZ)
#define MICROPY_FLOAT_IMPL              (MICROPY_FLOAT_IMPL_DOUBLE)
#define MICROPY_FLOAT_USE_NATIVE_FLT16  (0)       /* gcc has _Float16 on i686 only with SSE2 */
#define MICROPY_USE_INTERNAL_ERRNO      (1)       /* same numbers as common/abi.h (Linux), plus the socket ones */
#define MICROPY_ALLOC_PATH_MAX          (512)
#define MICROPY_READLINE_HISTORY_SIZE   (50)
#define MICROPY_USE_READLINE_HISTORY    (1)
#define MICROPY_HAL_HAS_VT100           (1)
#define MICROPY_ERROR_REPORTING         (MICROPY_ERROR_REPORTING_DETAILED)
#define MICROPY_WARNINGS                (1)
#define MICROPY_PY_SYS_PLATFORM         "vibeos"
#define MICROPY_HW_BOARD_NAME           "VibeOS"
#define MICROPY_HW_MCU_NAME             "i686"
#define MICROPY_PY_SYS_PATH_ARGV_DEFAULTS (0)     /* main.c fills sys.path and sys.argv */
#define MICROPY_PY_SYS_EXIT             (1)
#define MICROPY_PY_SYS_EXC_INFO         (1)
#define MICROPY_PY_SYS_EXECUTABLE       (1)
#define MICROPY_PY_SYS_STDFILES         (1)
#define MICROPY_PY_SYS_MAXSIZE          (1)
#define MICROPY_PYEXEC_ENABLE_EXIT_CODE_HANDLING (1)
#define MICROPY_MODULE_OVERRIDE_MAIN_IMPORT (1)
#define MICROPY_EPOCH_IS_1970           (1)
#define MICROPY_TIMESTAMP_IMPL          (MICROPY_TIMESTAMP_IMPL_TIME_T)

/* files: the VibeOS filesystem through VfsPosix mounted at / */
#define MICROPY_VFS                     (1)
#define MICROPY_VFS_POSIX               (1)
#define MICROPY_READER_VFS              (1)
#define MICROPY_PY_OS_STATVFS           (0)
/* our struct stat has no device or owner: os.stat() reports 0 for them (sb.st_uid -> sb.st_ino * 0) */
#define st_dev st_ino * 0
#define st_uid st_ino * 0
#define st_gid st_ino * 0
#define _DIRENT_HAVE_D_TYPE (1)                   /* os.ilistdir() reports file vs directory */
#define _DIRENT_HAVE_D_INO (1)
#define MICROPY_PY_OS_INCLUDEFILE       "user/bin/python/vibe_os.inc"
#define MICROPY_PY_OS_GETENV_PUTENV_UNSETENV (1)
#define MICROPY_PY_OS_SYSTEM            (1)
#define MICROPY_PY_OS_URANDOM           (1)
#define MICROPY_PY_OS_ERRNO             (1)
#define MICROPY_PY_OS_UNAME             (1)
#define MICROPY_PY_TIME_INCLUDEFILE     "user/bin/python/vibe_time.inc"
#define MICROPY_PY_TIME_GMTIME_LOCALTIME_MKTIME (0)  /* vibe_time.inc uses libc's, which know /etc/tz */
#define MICROPY_PY_TIME_TIME_TIME_NS    (1)
#define MICROPY_PY_TIME_CUSTOM_SLEEP    (1)
#define MICROPY_PY_RANDOM_SEED_INIT_FUNC (mp_random_seed_init())

/* not here: native code emitters, threads, hardware, sockets, asyncio (frozen Python), selected extmods */
#define MICROPY_EMIT_X86                (0)
#define MICROPY_PY_THREAD               (0)
#define MICROPY_PY_MACHINE              (0)
#define MICROPY_PY_SELECT               (0)
#define MICROPY_PY_ASYNCIO              (0)
#define MICROPY_PY_SOCKET               (0)
#define MICROPY_PY_NETWORK              (0)
#define MICROPY_PY_BINASCII_CRC32       (0)
#define MICROPY_PY_DEFLATE              (0)
#define MICROPY_PY_FRAMEBUF             (0)
#define MICROPY_PY_UCTYPES              (0)
#define MICROPY_PY_HASHLIB              (0)
#define MICROPY_PY_CRYPTOLIB            (0)
#define MICROPY_PY_WEBSOCKET            (0)
#define MICROPY_PY_WEBREPL              (0)
#define MICROPY_PY_ONEWIRE              (0)
#define MICROPY_PY_PLATFORM             (0)
#define MICROPY_PY_OS_DUPTERM           (0)
#define MICROPY_ENABLE_SCHEDULER        (1)

#define MP_STATE_PORT MP_STATE_VM
#define MICROPY_PORT_ROOT_POINTERS

typedef long mp_off_t;

extern const struct _mp_print_t mp_stderr_print;
#define MICROPY_ERROR_PRINTER (&mp_stderr_print)

unsigned long mp_random_seed_init(void);
