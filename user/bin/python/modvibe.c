/* modvibe.c - the `vibe` module: VibeOS system calls that os/time don't cover (same set as Lua's vibe module) */
#include <syscall.h>
#include <gui.h>

#include "py/mperrno.h"
#include "py/runtime.h"

static mp_obj_t vibe_sysinfo(void) {
    struct sysinfo si;
    if (sysinfo(&si) < 0) mp_raise_OSError(MP_EIO);
    mp_obj_t d = mp_obj_new_dict(9);
#define F(n) mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_##n), mp_obj_new_int_from_uint(si.n))
    F(uptime_ms); F(mem_total_kb); F(mem_free_kb); F(heap_total_kb); F(heap_used_kb);
    F(nprocs); F(idle_ms); F(screen_w); F(screen_h);
#undef F
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_0(vibe_sysinfo_obj, vibe_sysinfo);

static mp_obj_t vibe_ps(void) {                     /* [(pid, ppid, state, mem_kb, cpu_ms, name), ...] */
    static const qstr st[] = { MP_QSTR_running, MP_QSTR_ready, MP_QSTR_blocked, MP_QSTR_zombie };
    static struct procinfo p[128];
    int n = proclist(p, 128);
    if (n < 0) mp_raise_OSError(MP_EIO);
    mp_obj_t l = mp_obj_new_list(0, NULL);
    for (int i = 0; i < n; i++) {
        mp_obj_t t[6] = {
            MP_OBJ_NEW_SMALL_INT(p[i].pid), MP_OBJ_NEW_SMALL_INT(p[i].ppid),
            MP_OBJ_NEW_QSTR(p[i].state >= 0 && p[i].state < 4 ? st[p[i].state] : MP_QSTR__question_),
            mp_obj_new_int_from_uint(p[i].mem_kb), mp_obj_new_int_from_uint(p[i].cpu_ms), mp_obj_new_str_from_cstr(p[i].name),
        };
        mp_obj_list_append(l, mp_obj_new_tuple(6, t));
    }
    return l;
}
static MP_DEFINE_CONST_FUN_OBJ_0(vibe_ps_obj, vibe_ps);

static mp_obj_t vibe_beep(size_t n_args, const mp_obj_t *args) {      /* beep(hz, ms=200) */
    beep(mp_obj_get_int(args[0]), n_args > 1 ? mp_obj_get_int(args[1]) : 200);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(vibe_beep_obj, 1, 2, vibe_beep);

static mp_obj_t vibe_uptime(void) { return mp_obj_new_float(uptime_ms() / 1000.0); }
static MP_DEFINE_CONST_FUN_OBJ_0(vibe_uptime_obj, vibe_uptime);

static mp_obj_t vibe_notify(size_t n_args, const mp_obj_t *args) {    /* notify(title, body="") -> desktop toast */
    if (gui_notify(mp_obj_str_get_str(args[0]), n_args > 1 ? mp_obj_str_get_str(args[1]) : "", NULL) < 0) mp_raise_OSError(MP_EIO);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(vibe_notify_obj, 1, 2, vibe_notify);

static const mp_rom_map_elem_t vibe_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_vibe) },
    { MP_ROM_QSTR(MP_QSTR_sysinfo), MP_ROM_PTR(&vibe_sysinfo_obj) },
    { MP_ROM_QSTR(MP_QSTR_ps), MP_ROM_PTR(&vibe_ps_obj) },
    { MP_ROM_QSTR(MP_QSTR_beep), MP_ROM_PTR(&vibe_beep_obj) },
    { MP_ROM_QSTR(MP_QSTR_uptime), MP_ROM_PTR(&vibe_uptime_obj) },
    { MP_ROM_QSTR(MP_QSTR_notify), MP_ROM_PTR(&vibe_notify_obj) },
};
static MP_DEFINE_CONST_DICT(vibe_globals, vibe_globals_table);

const mp_obj_module_t mp_module_vibe = { .base = { &mp_type_module }, .globals = (mp_obj_dict_t *)&vibe_globals };
MP_REGISTER_MODULE(MP_QSTR_vibe, mp_module_vibe);
