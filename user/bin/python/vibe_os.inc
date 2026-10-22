/* vibe_os.inc - included by extmod/modos.c (MICROPY_PY_OS_INCLUDEFILE): the os functions the VFS doesn't cover */
#include <errno.h>
#include <stdlib.h>
#include "py/runtime.h"

static mp_obj_t mp_os_getenv(size_t n_args, const mp_obj_t *args) {
    const char *s = getenv(mp_obj_str_get_str(args[0]));
    if (!s) return n_args == 2 ? args[1] : mp_const_none;
    return mp_obj_new_str_from_cstr(s);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mp_os_getenv_obj, 1, 2, mp_os_getenv);

static mp_obj_t mp_os_putenv(mp_obj_t key, mp_obj_t val) {
    if (setenv(mp_obj_str_get_str(key), mp_obj_str_get_str(val), 1) < 0) mp_raise_OSError(errno);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(mp_os_putenv_obj, mp_os_putenv);

static mp_obj_t mp_os_unsetenv(mp_obj_t key) {
    if (unsetenv(mp_obj_str_get_str(key)) < 0) mp_raise_OSError(errno);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_os_unsetenv_obj, mp_os_unsetenv);

static mp_obj_t mp_os_system(mp_obj_t cmd) {        /* runs /bin/sh -c CMD, returns its exit status */
    int r = system(mp_obj_str_get_str(cmd));
    if (r < 0) mp_raise_OSError(errno);
    return MP_OBJ_NEW_SMALL_INT(r);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_os_system_obj, mp_os_system);

static mp_obj_t mp_os_errno(size_t n_args, const mp_obj_t *args) {
    if (!n_args) return MP_OBJ_NEW_SMALL_INT(errno);
    errno = mp_obj_get_int(args[0]);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mp_os_errno_obj, 0, 1, mp_os_errno);
