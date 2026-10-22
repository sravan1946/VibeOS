/* modselect.c - `select`: select() and poll() over the kernel's poll(). Objects are fds or anything with fileno()
 * (sockets, gui windows, files). The kernel polls at most 64 fds. */
#include "py/mperrno.h"
#include "py/objtuple.h"
#include "vibe_io.h"

#define MAXFD 64

static int fd_of(mp_obj_t o) {
    return mp_obj_get_int(mp_obj_is_int(o) ? o : mp_call_function_0(mp_load_attr(o, MP_QSTR_fileno)));
}

static int wait_ms(mp_obj_t t) { return t == mp_const_none ? -1 : (int)(mp_obj_get_float(t) * 1000 + 0.5); }

static mp_obj_t sel_select(size_t n, const mp_obj_t *a) {            /* select(rlist, wlist, xlist, timeout=None) -> (r, w, []) */
    struct pollfd p[MAXFD];
    mp_obj_t o[MAXFD];
    int k = 0, nr = 0;
    for (int l = 0; l < 2; l++) {
        size_t len;
        mp_obj_t *it;
        mp_obj_get_array(a[l], &len, &it);
        if (k + len > MAXFD) mp_raise_ValueError(MP_ERROR_TEXT("too many fds"));
        for (size_t i = 0; i < len; i++, k++) {
            o[k] = it[i];
            p[k] = (struct pollfd){ fd_of(it[i]), l ? POLLOUT : POLLIN, 0 };
        }
        if (!l) nr = k;
    }
    if (vpoll(p, k, n > 3 ? wait_ms(a[3]) : -1) < 0) mp_raise_OSError(errno);
    mp_obj_t res[3] = { mp_obj_new_list(0, NULL), mp_obj_new_list(0, NULL), mp_obj_new_list(0, NULL) };
    for (int i = 0; i < k; i++)
        if (p[i].revents & (p[i].events | POLLHUP)) mp_obj_list_append(res[i < nr ? 0 : 1], o[i]);
    return mp_obj_new_tuple(3, res);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(sel_select_obj, 3, 4, sel_select);

/* ---- poll objects: a dict of fd -> (obj, eventmask) ---- */
typedef struct { mp_obj_base_t base; mp_obj_t reg; } poll_t;
extern const mp_obj_type_t poll_type;

static mp_obj_t poll_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 0, 0, false);
    poll_t *s = mp_obj_malloc(poll_t, &poll_type);
    s->reg = mp_obj_new_dict(0);
    return MP_OBJ_FROM_PTR(s);
}

static mp_obj_t poll_register(size_t n, const mp_obj_t *a) {         /* register(obj, eventmask=POLLIN | POLLOUT); also modify() */
    mp_obj_t t[2] = { a[1], n > 2 ? a[2] : MP_OBJ_NEW_SMALL_INT(POLLIN | POLLOUT) };
    mp_obj_dict_store(((poll_t *)MP_OBJ_TO_PTR(a[0]))->reg, MP_OBJ_NEW_SMALL_INT(fd_of(a[1])), mp_obj_new_tuple(2, t));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(poll_register_obj, 2, 3, poll_register);

static mp_obj_t poll_unregister(mp_obj_t self, mp_obj_t obj) {
    mp_obj_t d = ((poll_t *)MP_OBJ_TO_PTR(self))->reg, k = MP_OBJ_NEW_SMALL_INT(fd_of(obj));
    if (!mp_map_lookup(mp_obj_dict_get_map(d), k, MP_MAP_LOOKUP)) mp_raise_msg(&mp_type_KeyError, NULL);
    mp_obj_dict_delete(d, k);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(poll_unregister_obj, poll_unregister);

static mp_obj_t poll_poll(size_t n, const mp_obj_t *a) {             /* poll(ms=-1) -> [(obj, events), ...] */
    mp_map_t *m = mp_obj_dict_get_map(((poll_t *)MP_OBJ_TO_PTR(a[0]))->reg);
    struct pollfd p[MAXFD];
    mp_obj_t o[MAXFD];
    int k = 0;
    for (size_t i = 0; i < m->alloc; i++) {
        if (!mp_map_slot_is_filled(m, i)) continue;
        if (k == MAXFD) mp_raise_ValueError(MP_ERROR_TEXT("too many fds"));
        mp_obj_t *t;
        mp_obj_get_array_fixed_n(m->table[i].value, 2, &t);
        o[k] = t[0];
        p[k++] = (struct pollfd){ mp_obj_get_int(m->table[i].key), mp_obj_get_int(t[1]), 0 };
    }
    int ms = n > 1 && a[1] != mp_const_none ? mp_obj_get_int(a[1]) : -1;
    if (vpoll(p, k, ms) < 0) mp_raise_OSError(errno);
    mp_obj_t l = mp_obj_new_list(0, NULL);
    for (int i = 0; i < k; i++)
        if (p[i].revents) mp_obj_list_append(l, mp_obj_new_tuple(2, (mp_obj_t[]){ o[i], MP_OBJ_NEW_SMALL_INT(p[i].revents) }));
    return l;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(poll_poll_obj, 1, 2, poll_poll);

static const mp_rom_map_elem_t poll_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_register), MP_ROM_PTR(&poll_register_obj) },
    { MP_ROM_QSTR(MP_QSTR_modify), MP_ROM_PTR(&poll_register_obj) },
    { MP_ROM_QSTR(MP_QSTR_unregister), MP_ROM_PTR(&poll_unregister_obj) },
    { MP_ROM_QSTR(MP_QSTR_poll), MP_ROM_PTR(&poll_poll_obj) },
};
static MP_DEFINE_CONST_DICT(poll_locals, poll_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(poll_type, MP_QSTR_poll, MP_TYPE_FLAG_NONE, make_new, poll_make_new, locals_dict, &poll_locals);

static const mp_rom_map_elem_t select_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_select) },
    { MP_ROM_QSTR(MP_QSTR_select), MP_ROM_PTR(&sel_select_obj) },
    { MP_ROM_QSTR(MP_QSTR_poll), MP_ROM_PTR(&poll_type) },
    { MP_ROM_QSTR(MP_QSTR_POLLIN), MP_ROM_INT(POLLIN) },
    { MP_ROM_QSTR(MP_QSTR_POLLOUT), MP_ROM_INT(POLLOUT) },
    { MP_ROM_QSTR(MP_QSTR_POLLERR), MP_ROM_INT(8) },
    { MP_ROM_QSTR(MP_QSTR_POLLHUP), MP_ROM_INT(POLLHUP) },
};
static MP_DEFINE_CONST_DICT(select_globals, select_globals_table);

const mp_obj_module_t mp_module_select = { .base = { &mp_type_module }, .globals = (mp_obj_dict_t *)&select_globals };
MP_REGISTER_MODULE(MP_QSTR_select, mp_module_select);
