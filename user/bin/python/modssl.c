/* modssl.c - `_ssl` (root/lib/python/ssl.py is the public module): a TLS client stream over libtls (BearSSL).
 * Certificates are always checked against /etc/ssl/cert.pem and the server name. Gaps: no server side, no
 * timeouts (reads and writes block), no settimeout/setblocking. */
#include <tls.h>

#include "py/mperrno.h"
#include "py/stream.h"
#include "vibe_io.h"

typedef struct { mp_obj_base_t base; struct tls *t; int fd; } ssl_t;
extern const mp_obj_type_t ssl_type;
int vibe_sock_fd(mp_obj_t o, bool detach);                          /* modsocket.c */

static mp_obj_t wrap_socket(size_t n, const mp_obj_t *args, mp_map_t *kw) {   /* wrap_socket(sock, server_hostname=host) */
    static const mp_arg_t allowed[] = { { MP_QSTR_server_hostname, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } } };
    mp_arg_val_t a[1];
    mp_arg_parse_all(n - 1, args + 1, kw, 1, allowed, a);
    if (a[0].u_obj == mp_const_none) mp_raise_ValueError(MP_ERROR_TEXT("server_hostname is required (certificates are always checked)"));
    int fd = vibe_sock_fd(args[0], false);
    struct tls *t = tls_open(fd, mp_obj_str_get_str(a[0].u_obj));
    if (!t) mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("TLS: %s"), tls_error());
    vibe_sock_fd(args[0], true);
    ssl_t *s = mp_obj_malloc_with_finaliser(ssl_t, &ssl_type);
    s->t = t;
    s->fd = fd;
    return MP_OBJ_FROM_PTR(s);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(wrap_socket_obj, 1, wrap_socket);

static ssl_t *S(mp_obj_t o) {
    ssl_t *s = MP_OBJ_TO_PTR(o);
    if (!s->t) mp_raise_OSError(EBADF);
    return s;
}

static mp_uint_t ssl_read(mp_obj_t self, void *buf, mp_uint_t size, int *err) {
    ssl_t *s = MP_OBJ_TO_PTR(self);
    if (!s->t) { *err = EBADF; return MP_STREAM_ERROR; }
    ssize_t r = tls_read(s->t, buf, size);
    if (r < 0) { *err = EIO; return MP_STREAM_ERROR; }
    return r;
}

static mp_uint_t ssl_write(mp_obj_t self, const void *buf, mp_uint_t size, int *err) {
    ssl_t *s = MP_OBJ_TO_PTR(self);
    if (!s->t) { *err = EBADF; return MP_STREAM_ERROR; }
    if (tls_write(s->t, buf, size) < 0) { *err = EIO; return MP_STREAM_ERROR; }
    return size;
}

static mp_uint_t ssl_ioctl(mp_obj_t self, mp_uint_t req, uintptr_t arg, int *err) {
    ssl_t *s = MP_OBJ_TO_PTR(self);
    if (req != MP_STREAM_CLOSE) { *err = EINVAL; return MP_STREAM_ERROR; }
    if (s->t) { tls_close(s->t); s->t = NULL; }
    return 0;
}

static mp_obj_t ssl_recv(mp_obj_t self, mp_obj_t len) {              /* b'' at end of stream */
    ssl_t *s = S(self);
    vstr_t v;
    vstr_init_len(&v, mp_obj_get_int(len));
    ssize_t r = tls_read(s->t, v.buf, v.len);
    if (r < 0) { vstr_clear(&v); mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("TLS: %s"), tls_error()); }
    v.len = r;
    return mp_obj_new_bytes_from_vstr(&v);
}
static MP_DEFINE_CONST_FUN_OBJ_2(ssl_recv_obj, ssl_recv);

static mp_obj_t ssl_sendall(mp_obj_t self, mp_obj_t data) {          /* also send(): TLS writes are all or nothing */
    mp_buffer_info_t b;
    mp_get_buffer_raise(data, &b, MP_BUFFER_READ);
    if (tls_write(S(self)->t, b.buf, b.len) < 0) mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("TLS: %s"), tls_error());
    return mp_obj_new_int(b.len);
}
static MP_DEFINE_CONST_FUN_OBJ_2(ssl_sendall_obj, ssl_sendall);

static mp_obj_t ssl_fileno(mp_obj_t self) { return MP_OBJ_NEW_SMALL_INT(S(self)->fd); }
static MP_DEFINE_CONST_FUN_OBJ_1(ssl_fileno_obj, ssl_fileno);

static mp_obj_t ssl_makefile(size_t n, const mp_obj_t *a) { return a[0]; }
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ssl_makefile_obj, 1, 3, ssl_makefile);

static const mp_rom_map_elem_t ssl_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_recv), MP_ROM_PTR(&ssl_recv_obj) },
    { MP_ROM_QSTR(MP_QSTR_send), MP_ROM_PTR(&ssl_sendall_obj) },
    { MP_ROM_QSTR(MP_QSTR_sendall), MP_ROM_PTR(&ssl_sendall_obj) },
    { MP_ROM_QSTR(MP_QSTR_fileno), MP_ROM_PTR(&ssl_fileno_obj) },
    { MP_ROM_QSTR(MP_QSTR_makefile), MP_ROM_PTR(&ssl_makefile_obj) },
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&mp_stream_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&mp_stream_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_readline), MP_ROM_PTR(&mp_stream_unbuffered_readline_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&mp_stream_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&mp_stream_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&mp_stream_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&mp_stream___exit___obj) },
};
static MP_DEFINE_CONST_DICT(ssl_locals, ssl_locals_table);

static const mp_stream_p_t ssl_stream_p = { .read = ssl_read, .write = ssl_write, .ioctl = ssl_ioctl };

MP_DEFINE_CONST_OBJ_TYPE(ssl_type, MP_QSTR_SSLSocket, MP_TYPE_FLAG_ITER_IS_STREAM, protocol, &ssl_stream_p, locals_dict, &ssl_locals);

static const mp_rom_map_elem_t ssl_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR__ssl) },
    { MP_ROM_QSTR(MP_QSTR_wrap_socket), MP_ROM_PTR(&wrap_socket_obj) },
    { MP_ROM_QSTR(MP_QSTR_SSLSocket), MP_ROM_PTR(&ssl_type) },
};
static MP_DEFINE_CONST_DICT(ssl_globals, ssl_globals_table);

const mp_obj_module_t mp_module_ssl = { .base = { &mp_type_module }, .globals = (mp_obj_dict_t *)&ssl_globals };
MP_REGISTER_MODULE(MP_QSTR__ssl, mp_module_ssl);
