/* tls.c - TLS client: BearSSL's full client profile, trust anchors parsed from a PEM bundle */
#include <tls.h>
#include <bearssl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#define CA_FILE "/etc/ssl/cert.pem"

struct tls {
    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    br_sslio_context io;
    int fd;
    unsigned char buf[BR_SSL_BUFSIZE_BIDI];
};

static char errbuf[96];
const char *tls_error(void) { return errbuf; }

/* ---- trust anchors ---- */
static br_x509_trust_anchor *tas;
static size_t ntas, cap;

struct vec { unsigned char *p; size_t n, cap; };

static void vec_add(void *ctx, const void *d, size_t n) {
    struct vec *v = ctx;
    if (v->n + n > v->cap) {
        size_t c = v->cap ? v->cap : 1024;
        while (c < v->n + n) c *= 2;
        unsigned char *np = realloc(v->p, c);
        if (!np) return;
        v->p = np; v->cap = c;
    }
    memcpy(v->p + v->n, d, n);
    v->n += n;
}

static void *memdup(const void *p, size_t n) { void *d = malloc(n ? n : 1); if (d) memcpy(d, p, n); return d; }

static void add_anchor(const unsigned char *der, size_t len) {
    struct vec dn = { 0 };
    br_x509_decoder_context dc;
    br_x509_decoder_init(&dc, vec_add, &dn);
    br_x509_decoder_push(&dc, der, len);
    const br_x509_pkey *pk = br_x509_decoder_get_pkey(&dc);
    if (!pk || (ntas == cap && !(tas = realloc(tas, (cap = cap ? cap * 2 : 64) * sizeof *tas)))) { free(dn.p); return; }
    br_x509_trust_anchor *ta = &tas[ntas];
    ta->dn.data = dn.p; ta->dn.len = dn.n;
    ta->flags = br_x509_decoder_isCA(&dc) ? BR_X509_TA_CA : 0;
    ta->pkey = *pk;
    if (pk->key_type == BR_KEYTYPE_RSA) {
        ta->pkey.key.rsa.n = memdup(pk->key.rsa.n, pk->key.rsa.nlen);
        ta->pkey.key.rsa.e = memdup(pk->key.rsa.e, pk->key.rsa.elen);
    } else if (pk->key_type == BR_KEYTYPE_EC) {
        ta->pkey.key.ec.q = memdup(pk->key.ec.q, pk->key.ec.qlen);
    } else { free(dn.p); return; }
    ntas++;
}

static int load_anchors(void) {
    int fd = open(CA_FILE, O_RDONLY);
    if (fd < 0) { snprintf(errbuf, sizeof errbuf, "no CA bundle (%s)", CA_FILE); return -1; }
    br_pem_decoder_context pc;
    br_pem_decoder_init(&pc);
    struct vec der = { 0 };
    int in_cert = 0;
    unsigned char b[4096];
    ssize_t n;
    while ((n = read(fd, b, sizeof b)) > 0)
        for (unsigned char *p = b; n > 0; ) {
            size_t k = br_pem_decoder_push(&pc, p, n);
            p += k; n -= k;
            switch (br_pem_decoder_event(&pc)) {
            case BR_PEM_BEGIN_OBJ:
                in_cert = !strcmp(br_pem_decoder_name(&pc), "CERTIFICATE") || !strcmp(br_pem_decoder_name(&pc), "TRUSTED CERTIFICATE");
                der.n = 0;
                br_pem_decoder_setdest(&pc, in_cert ? vec_add : 0, &der);
                break;
            case BR_PEM_END_OBJ:
                if (in_cert && der.n) add_anchor(der.p, der.n);
                in_cert = 0;
                break;
            case BR_PEM_ERROR:
                n = 0;
                break;
            }
        }
    close(fd);
    free(der.p);
    if (!ntas) { snprintf(errbuf, sizeof errbuf, "no certificates in %s", CA_FILE); return -1; }
    return 0;
}

/* ---- connection ---- */
static int sock_read(void *ctx, unsigned char *buf, size_t len) {
    for (;;) {
        ssize_t r = read(*(int *)ctx, buf, len);
        if (r > 0) return r;
        if (r < 0 && errno == EINTR) continue;
        return -1;
    }
}

static int sock_write(void *ctx, const unsigned char *buf, size_t len) {
    for (;;) {
        ssize_t r = write(*(int *)ctx, buf, len);
        if (r > 0) return r;
        if (r < 0 && errno == EINTR) continue;
        return -1;
    }
}

static void set_err(struct tls *t) {
    int e = br_ssl_engine_last_error(&t->sc.eng);
    const char *m = e == BR_ERR_X509_NOT_TRUSTED ? "certificate not trusted" :
                    e == BR_ERR_X509_EXPIRED ? "certificate expired or not yet valid (check the clock)" :
                    e == BR_ERR_X509_BAD_SERVER_NAME ? "certificate is for a different host" :
                    e == BR_ERR_IO ? "connection closed during handshake" :
                    e == BR_ERR_UNSUPPORTED_VERSION || e == BR_ERR_BAD_VERSION ? "server needs a TLS version we lack (1.0-1.2 only)" :
                    e == BR_ERR_BAD_CIPHER_SUITE ? "no common cipher suite" :
                    e >= BR_ERR_RECV_FATAL_ALERT && e < BR_ERR_SEND_FATAL_ALERT ? "server sent a fatal alert" : 0;
    if (m) snprintf(errbuf, sizeof errbuf, "%s", m);
    else snprintf(errbuf, sizeof errbuf, "TLS error %d", e);
}

struct tls *tls_open(int fd, const char *host) {
    if (!ntas && load_anchors() < 0) return 0;
    struct tls *t = malloc(sizeof *t);
    if (!t) { snprintf(errbuf, sizeof errbuf, "out of memory"); return 0; }
    t->fd = fd;
    br_ssl_client_init_full(&t->sc, &t->xc, tas, ntas);
    br_ssl_engine_set_buffer(&t->sc.eng, t->buf, sizeof t->buf, 1);
    if (!br_ssl_client_reset(&t->sc, host, 0)) { set_err(t); free(t); return 0; }
    br_sslio_init(&t->io, &t->sc.eng, sock_read, &t->fd, sock_write, &t->fd);
    if (br_sslio_flush(&t->io) < 0 || br_ssl_engine_current_state(&t->sc.eng) == BR_SSL_CLOSED) { set_err(t); free(t); return 0; }
    return t;
}

ssize_t tls_read(struct tls *t, void *buf, size_t n) {
    int r = br_sslio_read(&t->io, buf, n);
    if (r >= 0) return r;
    int e = br_ssl_engine_last_error(&t->sc.eng);
    if (e == BR_ERR_OK || e == BR_ERR_IO) return 0;     /* close_notify, or a server that just hung up */
    set_err(t);
    return -1;
}

ssize_t tls_write(struct tls *t, const void *buf, size_t n) {
    if (br_sslio_write_all(&t->io, buf, n) < 0 || br_sslio_flush(&t->io) < 0) { set_err(t); return -1; }
    return n;
}

void tls_close(struct tls *t) {
    if (!t) return;
    if (br_ssl_engine_current_state(&t->sc.eng) != BR_SSL_CLOSED) {
        br_ssl_engine_close(&t->sc.eng);
        br_sslio_flush(&t->io);                           /* best effort; don't wait for the peer's reply */
    }
    close(t->fd);
    free(t);
}
