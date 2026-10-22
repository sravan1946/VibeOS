/* stdio.c - buffered FILE streams. One buffer per stream, used either for pending writes
 * (buf[0..len)) or for read-ahead (buf[pos..len)); `dir` says which. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

int __vformat(void (*out)(void *, const char *, size_t), void *ctx, const char *fmt, va_list ap);

enum { F_EOF = 1, F_ERR = 2, F_STATIC = 4, F_APPEND = 8 };
enum { D_NONE, D_READ, D_WRITE };

struct FILE {
    int fd, flags, dir, mode;           /* mode: _IOFBF/_IOLBF/_IONBF, or -1 = decide on first write */
    int can_read, can_write;
    unsigned char *buf;
    size_t cap, pos, len;
    int unget;                          /* pushed-back char or -1 */
    int own_buf;
    struct FILE *next;
    char *tmpname;                      /* tmpfile(): unlinked on fclose */
};

static unsigned char stdin_buf[BUFSIZ], stdout_buf[BUFSIZ];
static struct FILE f_in = { 0, F_STATIC, D_NONE, _IOFBF, 1, 0, stdin_buf, BUFSIZ, 0, 0, -1, 0, NULL, NULL };
static struct FILE f_out = { 1, F_STATIC, D_NONE, -1, 0, 1, stdout_buf, BUFSIZ, 0, 0, -1, 0, &f_in, NULL };
static struct FILE f_err = { 2, F_STATIC, D_NONE, _IONBF, 0, 1, NULL, 0, 0, 0, -1, 0, &f_out, NULL };
FILE *stdin = &f_in, *stdout = &f_out, *stderr = &f_err;
static struct FILE *all = &f_err;

/* ---- writing ---- */

static int wflush(FILE *f) {
    size_t off = 0;
    while (off < f->len) {
        ssize_t r = write(f->fd, f->buf + off, f->len - off);
        if (r <= 0) { f->flags |= F_ERR; f->len = 0; return EOF; }
        off += r;
    }
    f->len = 0;
    return 0;
}

/* drop read-ahead, repositioning the fd to the logical position */
static void rdiscard(FILE *f) {
    if (f->len > f->pos) lseek(f->fd, -(off_t)(f->len - f->pos), SEEK_CUR);
    f->pos = f->len = 0;
    f->unget = -1;
}

static int prep_write(FILE *f) {
    if (!f->can_write) { f->flags |= F_ERR; errno = EBADF; return -1; }
    if (f->dir == D_READ) rdiscard(f);
    f->dir = D_WRITE;
    if (f->mode < 0) f->mode = isatty(f->fd) ? _IOLBF : _IOFBF;
    if (!f->buf && f->mode != _IONBF) {
        f->buf = malloc(BUFSIZ);
        if (!f->buf) f->mode = _IONBF; else { f->cap = BUFSIZ; f->own_buf = 1; }
    }
    return 0;
}

size_t fwrite(const void *p, size_t sz, size_t n, FILE *f) {
    size_t total = sz * n;
    const char *s = p;
    if (!total || prep_write(f) < 0) return 0;
    if (f->mode == _IONBF) {
        size_t off = 0;
        while (off < total) {
            ssize_t r = write(f->fd, s + off, total - off);
            if (r <= 0) { f->flags |= F_ERR; return off / sz; }
            off += r;
        }
        return n;
    }
    size_t left = total;
    int nl = f->mode == _IOLBF && memchr(s, '\n', total);
    while (left) {
        size_t room = f->cap - f->len, k = left < room ? left : room;
        memcpy(f->buf + f->len, s, k);
        f->len += k;
        s += k;
        left -= k;
        if (f->len == f->cap && wflush(f) < 0) return (total - left) / sz;
    }
    if (nl && wflush(f) < 0) return 0;
    return n;
}

int fputc(int c, FILE *f) {
    unsigned char ch = c;
    return fwrite(&ch, 1, 1, f) == 1 ? ch : EOF;
}
int putc(int c, FILE *f) { return fputc(c, f); }
int putchar(int c) { return fputc(c, stdout); }
int fputs(const char *s, FILE *f) { return fwrite(s, 1, strlen(s), f) == strlen(s) ? 0 : EOF; }
int puts(const char *s) { return fputs(s, stdout) < 0 || fputc('\n', stdout) < 0 ? EOF : 0; }

int fflush(FILE *f) {
    if (!f) {
        int r = 0;
        for (FILE *q = all; q; q = q->next) if (fflush(q) < 0) r = EOF;
        return r;
    }
    if (f->dir == D_WRITE) return wflush(f);
    if (f->dir == D_READ && f->can_write) rdiscard(f);
    return 0;
}

/* ---- reading ---- */

static int fill(FILE *f) {
    if (!f->can_read) { f->flags |= F_ERR; errno = EBADF; return -1; }
    if (f->dir == D_WRITE && wflush(f) < 0) return -1;
    f->dir = D_READ;
    if (f == stdin) fflush(stdout);                 /* show prompts before blocking */
    if (!f->buf) {
        f->buf = malloc(BUFSIZ);
        if (!f->buf) { f->flags |= F_ERR; return -1; }
        f->cap = BUFSIZ;
        f->own_buf = 1;
    }
    ssize_t r = read(f->fd, f->buf, f->cap);
    f->pos = f->len = 0;
    if (r < 0) { f->flags |= F_ERR; return -1; }
    if (r == 0) { f->flags |= F_EOF; return -1; }
    f->len = r;
    return 0;
}

int fgetc(FILE *f) {
    if (f->unget >= 0) { int c = f->unget; f->unget = -1; return c; }
    if (f->pos >= f->len || f->dir != D_READ) {
        f->pos = f->len = 0;
        if (fill(f) < 0) return EOF;
    }
    return f->buf[f->pos++];
}
int getc(FILE *f) { return fgetc(f); }
int getchar(void) { return fgetc(stdin); }

int ungetc(int c, FILE *f) {
    if (c == EOF || f->unget >= 0) return EOF;
    f->unget = (unsigned char)c;
    f->flags &= ~F_EOF;
    return c;
}

size_t fread(void *p, size_t sz, size_t n, FILE *f) {
    size_t want = sz * n, got = 0;
    char *d = p;
    if (!want) return 0;
    while (got < want) {
        int c;
        if (f->unget >= 0 || (f->dir == D_READ && f->pos < f->len)) {
            if (f->unget >= 0) { d[got++] = f->unget; f->unget = -1; continue; }
            size_t k = f->len - f->pos;
            if (k > want - got) k = want - got;
            memcpy(d + got, f->buf + f->pos, k);
            f->pos += k;
            got += k;
        } else if (want - got >= BUFSIZ && f->can_read && f->dir != D_WRITE) {
            ssize_t r = read(f->fd, d + got, want - got);    /* big request: skip the buffer */
            f->dir = D_READ;
            if (r < 0) { f->flags |= F_ERR; break; }
            if (r == 0) { f->flags |= F_EOF; break; }
            got += r;
        } else if ((c = fgetc(f)) == EOF) break;
        else d[got++] = c;
    }
    return got / sz;
}

char *fgets(char *s, int n, FILE *f) {
    int i = 0;
    if (n <= 0) return NULL;
    while (i < n - 1) {
        int c = fgetc(f);
        if (c == EOF) break;
        s[i++] = c;
        if (c == '\n') break;
    }
    if (!i) return NULL;
    s[i] = 0;
    return s;
}

ssize_t getdelim(char **line, size_t *n, int delim, FILE *f) {
    size_t len = 0;
    if (!*line || !*n) {
        *n = 120;
        *line = malloc(*n);
        if (!*line) return -1;
    }
    for (;;) {
        int c = fgetc(f);
        if (c == EOF) { if (!len) return -1; break; }
        if (len + 2 > *n) {
            char *p = realloc(*line, *n * 2);
            if (!p) return -1;
            *line = p;
            *n *= 2;
        }
        (*line)[len++] = c;
        if (c == delim) break;
    }
    (*line)[len] = 0;
    return len;
}
ssize_t getline(char **line, size_t *n, FILE *f) { return getdelim(line, n, '\n', f); }

/* ---- open / close / seek ---- */

FILE *fdopen(int fd, const char *mode) {
    int r = mode[0] == 'r', plus = strchr(mode, '+') != NULL;
    if (!strchr("rwa", mode[0])) { errno = EINVAL; return NULL; }
    FILE *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->fd = fd;
    f->mode = -1;
    f->unget = -1;
    f->can_read = r || plus;
    f->can_write = !r || plus;
    f->next = all;
    all = f;
    return f;
}

static int open_mode(const char *path, const char *mode) {
    int plus = strchr(mode, '+') != NULL, fl;
    switch (mode[0]) {
    case 'r': fl = plus ? O_RDWR : O_RDONLY; break;
    case 'w': fl = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC; break;
    case 'a': fl = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND; break;
    default: errno = EINVAL; return -1;
    }
    return open(path, fl, 0644);
}

FILE *fopen(const char *path, const char *mode) {
    int fd = open_mode(path, mode);
    if (fd < 0) return NULL;
    FILE *f = fdopen(fd, mode);
    if (!f) close(fd);
    return f;
}

/* reuse the FILE (and its buffer); fds 0-2 keep their number so stdin/stdout/stderr stay valid */
FILE *freopen(const char *path, const char *mode, FILE *f) {
    if (!path) { errno = EINVAL; return NULL; }          /* ponytail: no mode-change-only form */
    fflush(f);
    int fd = open_mode(path, mode), old = f->fd;
    if (fd < 0) { fclose(f); return NULL; }
    if (old <= 2) { dup2(fd, old); close(fd); fd = old; } else close(old);
    int plus = strchr(mode, '+') != NULL;
    f->fd = fd;
    f->flags &= F_STATIC;
    f->dir = D_NONE;
    f->pos = f->len = 0;
    f->unget = -1;
    f->can_read = mode[0] == 'r' || plus;
    f->can_write = mode[0] != 'r' || plus;
    if (f != stderr) f->mode = -1;
    return f;
}

char *tmpnam(char *buf) {
    static char sbuf[L_tmpnam];
    static unsigned n;
    struct stat st;
    if (!buf) buf = sbuf;
    for (int i = 0; i < TMP_MAX; i++) {
        snprintf(buf, L_tmpnam, "/tmp/t%x_%x", (unsigned)getpid() & 0xffff, ++n);
        if (stat(buf, &st) < 0) return buf;
    }
    return NULL;
}

FILE *tmpfile(void) {
    char name[L_tmpnam];
    if (!tmpnam(name)) return NULL;
    FILE *f = fopen(name, "w+");
    if (!f) return NULL;
    f->tmpname = strdup(name);
    return f;
}

int fclose(FILE *f) {
    int r = fflush(f);
    if (close(f->fd) < 0) r = EOF;
    for (FILE **pp = &all; *pp; pp = &(*pp)->next)
        if (*pp == f) { *pp = f->next; break; }
    if (f->own_buf) free(f->buf);
    if (f->tmpname) { unlink(f->tmpname); free(f->tmpname); }
    if (!(f->flags & F_STATIC)) free(f);
    return r;
}

int fseek(FILE *f, long off, int whence) {
    if (f->dir == D_WRITE && wflush(f) < 0) return -1;
    if (whence == SEEK_CUR && f->dir == D_READ) off -= f->len - f->pos;
    if (whence == SEEK_CUR && f->unget >= 0) off -= 1;
    f->pos = f->len = 0;
    f->unget = -1;
    f->dir = D_NONE;
    if (lseek(f->fd, off, whence) < 0) return -1;
    f->flags &= ~F_EOF;
    return 0;
}

long ftell(FILE *f) {
    off_t p = lseek(f->fd, 0, SEEK_CUR);
    if (p < 0) return -1;
    if (f->dir == D_WRITE) p += f->len;
    else if (f->dir == D_READ) p -= f->len - f->pos;
    if (f->unget >= 0) p--;
    return p;
}

void rewind(FILE *f) { fseek(f, 0, SEEK_SET); f->flags &= ~F_ERR; }
int feof(FILE *f) { return (f->flags & F_EOF) != 0; }
int ferror(FILE *f) { return (f->flags & F_ERR) != 0; }
void clearerr(FILE *f) { f->flags &= ~(F_EOF | F_ERR); }
int fileno(FILE *f) { return f->fd; }

int setvbuf(FILE *f, char *buf, int mode, size_t size) {
    if (f->len || f->pos != f->len) return -1;          /* only before first use */
    f->mode = mode;
    if (buf && size && mode != _IONBF) {
        if (f->own_buf) free(f->buf);
        f->buf = (unsigned char *)buf;
        f->cap = size;
        f->own_buf = 0;
    }
    return 0;
}
void setbuf(FILE *f, char *buf) { setvbuf(f, buf, buf ? _IOFBF : _IONBF, BUFSIZ); }

/* ---- printf family ---- */

struct fo { FILE *f; size_t n; char buf[256]; };

static void fo_flush(struct fo *o) {
    if (o->n) fwrite(o->buf, 1, o->n, o->f);
    o->n = 0;
}

static void fo_out(void *c, const char *s, size_t k) {
    struct fo *o = c;
    if (k >= sizeof o->buf) { fo_flush(o); fwrite(s, 1, k, o->f); return; }
    if (o->n + k > sizeof o->buf) fo_flush(o);
    memcpy(o->buf + o->n, s, k);
    o->n += k;
}

int vfprintf(FILE *f, const char *fmt, va_list ap) {
    struct fo o = { .f = f };
    int r = __vformat(fo_out, &o, fmt, ap);     /* batches stderr writes into one syscall */
    fo_flush(&o);
    return r;
}

int vprintf(const char *fmt, va_list ap) { return vfprintf(stdout, fmt, ap); }

int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return r;
}

/* ---- misc ---- */

void perror(const char *s) {
    if (s && *s) fprintf(stderr, "%s: %s\n", s, strerror(errno));
    else fprintf(stderr, "%s\n", strerror(errno));
}

int remove(const char *path) {
    if (unlink(path) == 0) return 0;
    if (errno == EISDIR) return rmdir(path);
    return -1;
}
