/* modsocket.c - the `socket` module over libc's sockets: AF_INET stream and datagram sockets, a timeout per socket,
 * names through the kernel resolver. Stream protocol too, so read/readline/makefile/select work.
 * Gaps: no shutdown() (the kernel has none), connect() ignores timeouts, no getsockname/getpeername. */
#include <string.h>
#include <sys/socket.h>

#include "py/mperrno.h"
#include "py/stream.h"
#include "vibe_io.h"

typedef struct { mp_obj_base_t base; int fd, timeout; } sock_t;      /* timeout in ms: -1 blocks, 0 never waits */
extern const mp_obj_type_t sock_type;

static mp_obj_t new_sock(int fd) {
    sock_t *s = mp_obj_malloc_with_finaliser(sock_t, &sock_type);
    s->fd = fd;
    s->timeout = -1;
    return MP_OBJ_FROM_PTR(s);
}

static sock_t *S(mp_obj_t o) {
    sock_t *s = MP_OBJ_TO_PTR(o);
    if (s->fd < 0) mp_raise_OSError(EBADF);
    return s;
}

static mp_obj_t make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 0, 3, false);
    int fd = socket(n_args > 0 ? mp_obj_get_int(args[0]) : AF_INET, n_args > 1 ? mp_obj_get_int(args[1]) : SOCK_STREAM,
                    n_args > 2 ? mp_obj_get_int(args[2]) : 0);
    if (fd < 0) mp_raise_OSError(errno);
    return new_sock(fd);
}

/* ---- addresses: (host, port), host a dotted quad or a name; '' or '0.0.0.0' is any ---- */
static void get_addr(mp_obj_t o, struct sockaddr_in *a) {
    size_t n;
    mp_obj_t *t;
    mp_obj_get_array(o, &n, &t);
    if (n != 2) mp_raise_TypeError(MP_ERROR_TEXT("address is (host, port)"));
    const char *h = mp_obj_str_get_str(t[0]);
    uint32_t ip = 0;
    if (*h && strcmp(h, "0.0.0.0") && resolve(h, &ip) < 0) mp_raise_OSError(errno);
    *a = (struct sockaddr_in){ AF_INET, htons(mp_obj_get_int(t[1])), ip, { 0 } };
}

static mp_obj_t addr_obj(const struct sockaddr_in *a) {
    char b[16];
    mp_obj_t t[2] = { mp_obj_new_str_from_cstr(inet_ntoa_r(a->sin_addr, b)), MP_OBJ_NEW_SMALL_INT(ntohs(a->sin_port)) };
    return mp_obj_new_tuple(2, t);
}

/* ---- timeouts: 0 or an errno (EAGAIN for non-blocking, ETIMEDOUT) once ev is ready or the time is up ---- */
static int ready(sock_t *s, int ev) {
    if (s->timeout < 0) return 0;
    struct pollfd p = { s->fd, ev, 0 };
    int r = vpoll(&p, 1, s->timeout);
    return r < 0 ? errno : r ? 0 : s->timeout ? ETIMEDOUT : EAGAIN;
}
static void check(int err) { if (err) mp_raise_OSError(err); }

static ssize_t rd(sock_t *s, void *buf, size_t n, int flags, struct sockaddr_in *from) {   /* -1 + errno */
    int e = ready(s, POLLIN);
    if (e) { errno = e; return -1; }
    for (;;) {
        socklen_t l = sizeof *from;
        ssize_t r = recvfrom(s->fd, buf, n, flags, (struct sockaddr *)from, &l);
        if (r >= 0 || errno != EINTR) return r;
        mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS);
    }
}

static ssize_t wr(sock_t *s, const void *buf, size_t n, const struct sockaddr_in *to) {
    int e = ready(s, POLLOUT);
    if (e) { errno = e; return -1; }
    for (;;) {
        ssize_t r = sendto(s->fd, buf, n, 0, (const struct sockaddr *)to, to ? sizeof *to : 0);
        if (r >= 0 || errno != EINTR) return r;
        mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS);
    }
}

/* ---- methods ---- */
static mp_obj_t sock_connect(mp_obj_t self, mp_obj_t addr) {
    struct sockaddr_in a;
    get_addr(addr, &a);
    if (connect(S(self)->fd, (struct sockaddr *)&a, sizeof a) < 0) mp_raise_OSError(errno);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sock_connect_obj, sock_connect);

static mp_obj_t sock_bind(mp_obj_t self, mp_obj_t addr) {
    struct sockaddr_in a;
    get_addr(addr, &a);
    if (bind(S(self)->fd, (struct sockaddr *)&a, sizeof a) < 0) mp_raise_OSError(errno);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sock_bind_obj, sock_bind);

static mp_obj_t sock_listen(size_t n, const mp_obj_t *a) {
    if (listen(S(a[0])->fd, n > 1 ? mp_obj_get_int(a[1]) : 5) < 0) mp_raise_OSError(errno);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(sock_listen_obj, 1, 2, sock_listen);

static mp_obj_t sock_accept(mp_obj_t self) {                         /* -> (socket, (host, port)) */
    sock_t *s = S(self);
    struct sockaddr_in a;
    socklen_t l = sizeof a;
    check(ready(s, POLLIN));
    int fd;
    while ((fd = accept(s->fd, (struct sockaddr *)&a, &l)) < 0) {
        if (errno != EINTR) mp_raise_OSError(errno);
        mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS);
    }
    mp_obj_t t[2] = { new_sock(fd), addr_obj(&a) };
    return mp_obj_new_tuple(2, t);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sock_accept_obj, sock_accept);

static mp_obj_t sock_recv(size_t n, const mp_obj_t *a) {             /* recv(n, flags=0) -> bytes, b'' at end of stream */
    sock_t *s = S(a[0]);
    vstr_t v;
    vstr_init_len(&v, mp_obj_get_int(a[1]));
    ssize_t r = rd(s, v.buf, v.len, n > 2 ? mp_obj_get_int(a[2]) : 0, NULL);
    if (r < 0) { vstr_clear(&v); mp_raise_OSError(errno); }
    v.len = r;
    return mp_obj_new_bytes_from_vstr(&v);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(sock_recv_obj, 2, 3, sock_recv);

static mp_obj_t sock_recvfrom(mp_obj_t self, mp_obj_t len) {         /* -> (bytes, (host, port)) */
    sock_t *s = S(self);
    struct sockaddr_in from = { 0 };
    vstr_t v;
    vstr_init_len(&v, mp_obj_get_int(len));
    ssize_t r = rd(s, v.buf, v.len, 0, &from);
    if (r < 0) { vstr_clear(&v); mp_raise_OSError(errno); }
    v.len = r;
    mp_obj_t t[2] = { mp_obj_new_bytes_from_vstr(&v), addr_obj(&from) };
    return mp_obj_new_tuple(2, t);
}
static MP_DEFINE_CONST_FUN_OBJ_2(sock_recvfrom_obj, sock_recvfrom);

static mp_obj_t sock_send(size_t n, const mp_obj_t *a) {             /* send(data) -> bytes sent */
    mp_buffer_info_t b;
    mp_get_buffer_raise(a[1], &b, MP_BUFFER_READ);
    ssize_t r = wr(S(a[0]), b.buf, b.len, NULL);
    if (r < 0) mp_raise_OSError(errno);
    return MP_OBJ_NEW_SMALL_INT(r);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(sock_send_obj, 2, 3, sock_send);

static mp_obj_t sock_sendall(size_t n, const mp_obj_t *a) {
    sock_t *s = S(a[0]);
    mp_buffer_info_t b;
    mp_get_buffer_raise(a[1], &b, MP_BUFFER_READ);
    for (size_t done = 0; done < b.len; ) {
        ssize_t r = wr(s, (char *)b.buf + done, b.len - done, NULL);
        if (r < 0) mp_raise_OSError(errno);
        done += r;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(sock_sendall_obj, 2, 3, sock_sendall);

static mp_obj_t sock_sendto(size_t n, const mp_obj_t *a) {           /* sendto(data, addr) or sendto(data, flags, addr) */
    struct sockaddr_in to;
    mp_buffer_info_t b;
    mp_get_buffer_raise(a[1], &b, MP_BUFFER_READ);
    get_addr(a[n - 1], &to);
    ssize_t r = wr(S(a[0]), b.buf, b.len, &to);
    if (r < 0) mp_raise_OSError(errno);
    return MP_OBJ_NEW_SMALL_INT(r);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(sock_sendto_obj, 3, 4, sock_sendto);

static mp_obj_t sock_settimeout(mp_obj_t self, mp_obj_t t) {         /* seconds; None blocks, 0 never waits */
    S(self)->timeout = t == mp_const_none ? -1 : (int)(mp_obj_get_float(t) * 1000 + 0.5);
    if (S(self)->timeout < -1) mp_raise_ValueError(MP_ERROR_TEXT("timeout must be >= 0"));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sock_settimeout_obj, sock_settimeout);

static mp_obj_t sock_gettimeout(mp_obj_t self) {
    int t = S(self)->timeout;
    return t < 0 ? mp_const_none : mp_obj_new_float(t / 1000.0);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sock_gettimeout_obj, sock_gettimeout);

static mp_obj_t sock_setblocking(mp_obj_t self, mp_obj_t b) {
    S(self)->timeout = mp_obj_is_true(b) ? -1 : 0;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sock_setblocking_obj, sock_setblocking);

static mp_obj_t sock_setsockopt(size_t n, const mp_obj_t *a) { return mp_const_none; }   /* SO_REUSEADDR etc: nothing to set */
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(sock_setsockopt_obj, 4, 4, sock_setsockopt);

static mp_obj_t sock_fileno(mp_obj_t self) { return MP_OBJ_NEW_SMALL_INT(S(self)->fd); }
static MP_DEFINE_CONST_FUN_OBJ_1(sock_fileno_obj, sock_fileno);

static mp_obj_t sock_makefile(size_t n, const mp_obj_t *a) { return a[0]; }   /* the socket is already a stream */
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(sock_makefile_obj, 1, 3, sock_makefile);

/* ---- stream protocol ---- */
static mp_uint_t sock_read(mp_obj_t self, void *buf, mp_uint_t size, int *err) {
    sock_t *s = MP_OBJ_TO_PTR(self);
    ssize_t r = s->fd < 0 ? (errno = EBADF, -1) : rd(s, buf, size, 0, NULL);
    if (r < 0) { *err = errno; return MP_STREAM_ERROR; }
    return r;
}

static mp_uint_t sock_write(mp_obj_t self, const void *buf, mp_uint_t size, int *err) {
    sock_t *s = MP_OBJ_TO_PTR(self);
    ssize_t r = s->fd < 0 ? (errno = EBADF, -1) : wr(s, buf, size, NULL);
    if (r < 0) { *err = errno; return MP_STREAM_ERROR; }
    return r;
}

static mp_uint_t sock_ioctl(mp_obj_t self, mp_uint_t req, uintptr_t arg, int *err) {
    sock_t *s = MP_OBJ_TO_PTR(self);
    if (req != MP_STREAM_CLOSE) { *err = EINVAL; return MP_STREAM_ERROR; }
    if (s->fd >= 0) { close(s->fd); s->fd = -1; }
    return 0;
}

static const mp_rom_map_elem_t sock_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_connect), MP_ROM_PTR(&sock_connect_obj) },
    { MP_ROM_QSTR(MP_QSTR_bind), MP_ROM_PTR(&sock_bind_obj) },
    { MP_ROM_QSTR(MP_QSTR_listen), MP_ROM_PTR(&sock_listen_obj) },
    { MP_ROM_QSTR(MP_QSTR_accept), MP_ROM_PTR(&sock_accept_obj) },
    { MP_ROM_QSTR(MP_QSTR_recv), MP_ROM_PTR(&sock_recv_obj) },
    { MP_ROM_QSTR(MP_QSTR_recvfrom), MP_ROM_PTR(&sock_recvfrom_obj) },
    { MP_ROM_QSTR(MP_QSTR_send), MP_ROM_PTR(&sock_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_sendall), MP_ROM_PTR(&sock_sendall_obj) },
    { MP_ROM_QSTR(MP_QSTR_sendto), MP_ROM_PTR(&sock_sendto_obj) },
    { MP_ROM_QSTR(MP_QSTR_settimeout), MP_ROM_PTR(&sock_settimeout_obj) },
    { MP_ROM_QSTR(MP_QSTR_gettimeout), MP_ROM_PTR(&sock_gettimeout_obj) },
    { MP_ROM_QSTR(MP_QSTR_setblocking), MP_ROM_PTR(&sock_setblocking_obj) },
    { MP_ROM_QSTR(MP_QSTR_setsockopt), MP_ROM_PTR(&sock_setsockopt_obj) },
    { MP_ROM_QSTR(MP_QSTR_fileno), MP_ROM_PTR(&sock_fileno_obj) },
    { MP_ROM_QSTR(MP_QSTR_makefile), MP_ROM_PTR(&sock_makefile_obj) },
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&mp_stream_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&mp_stream_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_readline), MP_ROM_PTR(&mp_stream_unbuffered_readline_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&mp_stream_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&mp_stream_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&mp_stream_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&mp_stream___exit___obj) },
};
static MP_DEFINE_CONST_DICT(sock_locals, sock_locals_table);

static const mp_stream_p_t sock_stream_p = { .read = sock_read, .write = sock_write, .ioctl = sock_ioctl };

MP_DEFINE_CONST_OBJ_TYPE(sock_type, MP_QSTR_socket, MP_TYPE_FLAG_ITER_IS_STREAM,
    make_new, make_new, protocol, &sock_stream_p, locals_dict, &sock_locals);

/* for _ssl: a socket's fd, optionally taken over (the TLS context then owns and closes it) */
int vibe_sock_fd(mp_obj_t o, bool detach) {
    if (!mp_obj_is_type(o, &sock_type)) mp_raise_TypeError(MP_ERROR_TEXT("socket expected"));
    sock_t *s = S(o);
    int fd = s->fd;
    if (detach) s->fd = -1;
    return fd;
}

/* ---- module ---- */
static mp_obj_t mod_getaddrinfo(size_t n, const mp_obj_t *a) {       /* -> [(AF_INET, type, proto, '', (ip, port))] */
    uint32_t ip = 0;
    const char *h = mp_obj_str_get_str(a[0]);
    if (resolve(h, &ip) < 0) mp_raise_OSError(errno);
    struct sockaddr_in sa = { AF_INET, htons(a[1] == mp_const_none ? 0 : mp_obj_get_int(a[1])), ip, { 0 } };
    int type = n > 3 && mp_obj_get_int(a[3]) ? mp_obj_get_int(a[3]) : SOCK_STREAM;
    mp_obj_t t[5] = { MP_OBJ_NEW_SMALL_INT(AF_INET), MP_OBJ_NEW_SMALL_INT(type), MP_OBJ_NEW_SMALL_INT(type == SOCK_STREAM ? 6 : 17),
                      MP_OBJ_NEW_QSTR(MP_QSTR_), addr_obj(&sa) };
    return mp_obj_new_list(1, (mp_obj_t[]){ mp_obj_new_tuple(5, t) });
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_getaddrinfo_obj, 2, 6, mod_getaddrinfo);

static mp_obj_t mod_gethostbyname(mp_obj_t name) {
    uint32_t ip;
    char b[16];
    if (resolve(mp_obj_str_get_str(name), &ip) < 0) mp_raise_OSError(errno);
    return mp_obj_new_str_from_cstr(inet_ntoa_r(ip, b));
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_gethostbyname_obj, mod_gethostbyname);

static mp_obj_t mod_inet_aton(mp_obj_t s) {                          /* '1.2.3.4' -> b'\x01\x02\x03\x04' */
    uint32_t ip;
    if (!inet_aton(mp_obj_str_get_str(s), &ip)) mp_raise_OSError(EINVAL);
    return mp_obj_new_bytes((byte *)&ip, 4);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_inet_aton_obj, mod_inet_aton);

static mp_obj_t mod_inet_ntoa(mp_obj_t b) {
    mp_buffer_info_t i;
    char s[16];
    uint32_t ip;
    mp_get_buffer_raise(b, &i, MP_BUFFER_READ);
    if (i.len != 4) mp_raise_ValueError(MP_ERROR_TEXT("4 bytes expected"));
    memcpy(&ip, i.buf, 4);
    return mp_obj_new_str_from_cstr(inet_ntoa_r(ip, s));
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_inet_ntoa_obj, mod_inet_ntoa);

#define C(n, v) { MP_ROM_QSTR(MP_QSTR_##n), MP_ROM_INT(v) }
static const mp_rom_map_elem_t socket_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_socket) },
    { MP_ROM_QSTR(MP_QSTR_socket), MP_ROM_PTR(&sock_type) },
    { MP_ROM_QSTR(MP_QSTR_error), MP_ROM_PTR(&mp_type_OSError) },
    { MP_ROM_QSTR(MP_QSTR_getaddrinfo), MP_ROM_PTR(&mod_getaddrinfo_obj) },
    { MP_ROM_QSTR(MP_QSTR_gethostbyname), MP_ROM_PTR(&mod_gethostbyname_obj) },
    { MP_ROM_QSTR(MP_QSTR_inet_aton), MP_ROM_PTR(&mod_inet_aton_obj) },
    { MP_ROM_QSTR(MP_QSTR_inet_ntoa), MP_ROM_PTR(&mod_inet_ntoa_obj) },
    C(AF_INET, AF_INET), C(SOCK_STREAM, SOCK_STREAM), C(SOCK_DGRAM, SOCK_DGRAM),
    C(IPPROTO_TCP, 6), C(IPPROTO_UDP, 17), C(SOL_SOCKET, 1), C(SO_REUSEADDR, 2), C(SOMAXCONN, 32),
    C(MSG_DONTWAIT, MSG_DONTWAIT),
};
static MP_DEFINE_CONST_DICT(socket_globals, socket_globals_table);

const mp_obj_module_t mp_module_socket = { .base = { &mp_type_module }, .globals = (mp_obj_dict_t *)&socket_globals };
MP_REGISTER_MODULE(MP_QSTR_socket, mp_module_socket);
