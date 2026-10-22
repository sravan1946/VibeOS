/* main.c - /bin/python: MicroPython (third_party/micropython) on VibeOS.
 *   python                 REPL (line editing, history, Tab completion, auto-indent, Ctrl-E paste mode)
 *   python FILE [ARGS]     run a script; sys.argv = [FILE, ARGS...]
 *   python -c CODE [ARGS]  run CODE
 *   python -i ...          REPL after the script */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <syscall.h>

#include "py/builtin.h"
#include "py/compile.h"
#include "py/cstack.h"
#include "py/gc.h"
#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/objstr.h"
#include "py/runtime.h"
#include "py/stackctrl.h"
#include "extmod/vfs.h"
#include "extmod/vfs_posix.h"
#include "shared/runtime/gchelper.h"
#include "shared/runtime/pyexec.h"
#include "genhdr/mpversion.h"

#define INITIAL_HEAP (1024 * 1024)        /* grows on demand: gc_get_max_new_split */
#define STACK_LIMIT (4 * 1024 * 1024)     /* user stacks grow to 8 MiB */

extern mp_obj_str_t mp_sys_executable_obj;     /* py/modsys.c */

static void stderr_strn(void *env, const char *s, size_t n) { write(2, s, n); }
const mp_print_t mp_stderr_print = { NULL, stderr_strn };

static void set_argv(char **argv, int argc, int from) {
    for (int i = from; i < argc; i++) mp_obj_list_append(mp_sys_argv, mp_obj_new_str_from_cstr(argv[i]));
}

static int exit_code(int ret) {           /* pyexec result -> process status */
    return ret & PYEXEC_FORCED_EXIT ? ret & 0xff : ret;
}

static void mount_root(void) {            /* the whole filesystem as VfsPosix at /, current dir included */
    mp_obj_t args[2] = {
        MP_OBJ_TYPE_GET_SLOT(&mp_type_vfs_posix, make_new)(&mp_type_vfs_posix, 0, 0, NULL),
        MP_OBJ_NEW_QSTR(MP_QSTR__slash_),
    };
    mp_vfs_mount(2, args, (mp_map_t *)&mp_const_empty_map);
    MP_STATE_VM(vfs_cur) = MP_STATE_VM(vfs_mount_table);
}

MP_NOINLINE static int run(int argc, char **argv) {
    static char heap0[INITIAL_HEAP] __attribute__((aligned(16)));
    gc_init(heap0, heap0 + sizeof heap0);
    mp_init();
    signal(SIGPIPE, SIG_IGN);
    mount_root();

    mp_sys_path = mp_obj_new_list(0, NULL);
    mp_obj_list_append(mp_sys_path, MP_OBJ_NEW_QSTR(MP_QSTR_));
    mp_obj_list_append(mp_sys_path, mp_obj_new_str_from_cstr("/lib/python"));
    mp_obj_list_init(MP_OBJ_TO_PTR(mp_sys_argv), 0);
    mp_obj_str_set_data(&mp_sys_executable_obj, (const byte *)"/bin/python", 11);

    int ret = -1, a = 1;
    bool inspect = false;
    for (; a < argc && argv[a][0] == '-'; a++) {
        if (!strcmp(argv[a], "-i")) inspect = true;
        else if (!strcmp(argv[a], "-c") && a + 1 < argc) {
            mp_obj_list_append(mp_sys_argv, mp_obj_new_str_from_cstr("-c"));   /* as in CPython */
            set_argv(argv, argc, a + 2);
            vstr_t code = { .buf = argv[a + 1], .len = strlen(argv[a + 1]) };
            ret = exit_code(pyexec_vstr(&code, true));
            a = argc;
            break;
        } else if (!strcmp(argv[a], "--version")) {
            printf("MicroPython %s (%s); %s\n", MICROPY_GIT_TAG, MICROPY_BUILD_DATE, MICROPY_BANNER_MACHINE);
            return 0;
        } else {
            fprintf(stderr, "usage: python [-i] [-c CODE | FILE] [ARGS...]\n");
            return argv[a][1] == 'h' ? 0 : 2;
        }
    }
    if (a < argc) {
        char *dir = realpath(argv[a], NULL);
        if (!dir) {
            fprintf(stderr, "python: can't open file '%s': %s\n", argv[a], strerror(errno));
            return 2;
        }
        *strrchr(dir, '/') = 0;                         /* the script's directory replaces '' in sys.path */
        mp_obj_list_store(mp_sys_path, MP_OBJ_NEW_SMALL_INT(0), mp_obj_new_str_from_cstr(*dir ? dir : "/"));
        free(dir);
        set_argv(argv, argc, a);
        ret = exit_code(pyexec_file(argv[a]));
    }
    if (ret < 0 || (inspect && !(ret & PYEXEC_FORCED_EXIT))) {
        if (ret < 0) mp_obj_list_append(mp_sys_argv, MP_OBJ_NEW_QSTR(MP_QSTR_));
        for (;;) {
            int r = pyexec_mode_kind == PYEXEC_MODE_RAW_REPL ? pyexec_raw_repl() : pyexec_friendly_repl();
            if (r & PYEXEC_FORCED_EXIT) { ret = r & 0xff; break; }
        }
    }
    gc_sweep_all();
    mp_deinit();
    return ret;
}

int main(int argc, char **argv) {
    mp_cstack_init_with_sp_here(STACK_LIMIT);    /* before any other locals, so the GC scans the whole stack */
    return run(argc, argv);
}

void gc_collect(void) {
    gc_collect_start();
    gc_helper_collect_regs_and_stack();
    gc_collect_end();
}

/* the split heap asks how much more it may take: half of what the kernel has free */
size_t gc_get_max_new_split(void) {
    struct sysinfo si;
    return sysinfo(&si) == 0 ? (size_t)si.mem_free_kb * 512 : 0;
}

void nlr_jump_fail(void *val) {
    fprintf(stderr, "python: FATAL: uncaught NLR %p\n", val);
    exit(1);
}
