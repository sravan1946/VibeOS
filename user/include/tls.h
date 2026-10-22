/* tls.h - TLS 1.2 client over a connected TCP fd (BearSSL in libtls.a).
 * Certificates are checked against /etc/ssl/cert.pem and the server name. */
#pragma once
#include <sys/types.h>

struct tls;
struct tls *tls_open(int fd, const char *host);           /* handshake; NULL on failure, see tls_error() */
ssize_t tls_read(struct tls *t, void *buf, size_t n);     /* blocks; 0 at end of stream, -1 on error */
ssize_t tls_write(struct tls *t, const void *buf, size_t n);
void tls_close(struct tls *t);                            /* close_notify, close(fd), free */
const char *tls_error(void);                              /* why the last call failed */
