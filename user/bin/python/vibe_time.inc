/* vibe_time.inc - included by extmod/modtime.c (MICROPY_PY_TIME_INCLUDEFILE): wall clock and calendar through
 * libc, so localtime/mktime follow /etc/tz like the rest of the system */
#include <time.h>
#include <sys/time.h>
#include "py/runtime.h"
#include "shared/timeutils/timeutils.h"

static mp_obj_t mp_time_time_get(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return mp_obj_new_float(tv.tv_sec + tv.tv_usec / 1e6);
}

static void mp_time_sleep(mp_obj_t secs) {           /* in slices, so ^C interrupts a long sleep */
    mp_float_t s = mp_obj_get_float(secs);
    if (s < 0) mp_raise_ValueError(MP_ERROR_TEXT("sleep length must be non-negative"));
    mp_uint_t end = mp_hal_ticks_ms() + (mp_uint_t)(s * 1000 + 0.5);
    for (;;) {
        mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS);
        mp_int_t left = (mp_int_t)(end - mp_hal_ticks_ms());
        if (left <= 0) break;
        mp_hal_delay_ms(left < 50 ? left : 50);
    }
}

static struct tm tm_from_tuple(mp_obj_t tuple, size_t *len) {
    mp_obj_t *e;
    mp_obj_get_array(tuple, len, &e);
    if (*len < 8 || *len > 9) mp_raise_TypeError(MP_ERROR_TEXT("time tuple must have 8 or 9 items"));
    return (struct tm){
        .tm_year = mp_obj_get_int(e[0]) - 1900, .tm_mon = mp_obj_get_int(e[1]) - 1, .tm_mday = mp_obj_get_int(e[2]),
        .tm_hour = mp_obj_get_int(e[3]), .tm_min = mp_obj_get_int(e[4]), .tm_sec = mp_obj_get_int(e[5]),
        .tm_wday = (mp_obj_get_int(e[6]) + 1) % 7, .tm_yday = mp_obj_get_int(e[7]) - 1,
    };
}

static mp_obj_t tm_tuple(size_t n_args, const mp_obj_t *args, struct tm *(*conv)(const time_t *, struct tm *)) {
    time_t t = n_args == 0 || args[0] == mp_const_none ? time(NULL) : (time_t)timeutils_obj_get_timestamp(args[0]);
    struct tm tm;
    conv(&t, &tm);
    mp_obj_t items[9] = {
        MP_OBJ_NEW_SMALL_INT(tm.tm_year + 1900), MP_OBJ_NEW_SMALL_INT(tm.tm_mon + 1), MP_OBJ_NEW_SMALL_INT(tm.tm_mday),
        MP_OBJ_NEW_SMALL_INT(tm.tm_hour), MP_OBJ_NEW_SMALL_INT(tm.tm_min), MP_OBJ_NEW_SMALL_INT(tm.tm_sec),
        MP_OBJ_NEW_SMALL_INT((tm.tm_wday + 6) % 7), MP_OBJ_NEW_SMALL_INT(tm.tm_yday + 1), MP_OBJ_NEW_SMALL_INT(0),
    };
    return mp_obj_new_tuple(9, items);
}

static mp_obj_t vibe_gmtime(size_t n_args, const mp_obj_t *args) { return tm_tuple(n_args, args, gmtime_r); }
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(vibe_gmtime_obj, 0, 1, vibe_gmtime);
static mp_obj_t vibe_localtime(size_t n_args, const mp_obj_t *args) { return tm_tuple(n_args, args, localtime_r); }
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(vibe_localtime_obj, 0, 1, vibe_localtime);

static mp_obj_t vibe_mktime(mp_obj_t tuple) {
    size_t len;
    struct tm tm = tm_from_tuple(tuple, &len);
    return timeutils_obj_from_timestamp(mktime(&tm));
}
static MP_DEFINE_CONST_FUN_OBJ_1(vibe_mktime_obj, vibe_mktime);

static mp_obj_t vibe_strftime(size_t n_args, const mp_obj_t *args) {   /* strftime(fmt[, tuple]), local time by default */
    struct tm tm;
    if (n_args == 1) {
        time_t t = time(NULL);
        localtime_r(&t, &tm);
    } else {
        size_t len;
        tm = tm_from_tuple(args[1], &len);
    }
    char buf[256];
    size_t n = strftime(buf, sizeof buf, mp_obj_str_get_str(args[0]), &tm);
    return mp_obj_new_str(buf, n);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(vibe_strftime_obj, 1, 2, vibe_strftime);

#define MICROPY_PY_TIME_EXTRA_GLOBALS \
    { MP_ROM_QSTR(MP_QSTR_gmtime), MP_ROM_PTR(&vibe_gmtime_obj) }, \
    { MP_ROM_QSTR(MP_QSTR_localtime), MP_ROM_PTR(&vibe_localtime_obj) }, \
    { MP_ROM_QSTR(MP_QSTR_mktime), MP_ROM_PTR(&vibe_mktime_obj) }, \
    { MP_ROM_QSTR(MP_QSTR_strftime), MP_ROM_PTR(&vibe_strftime_obj) },
