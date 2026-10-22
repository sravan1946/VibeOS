#pragma once
#include <stddef.h>
#include <stdarg.h>
#include <sys/types.h>
typedef struct FILE FILE;
extern FILE *stdin, *stdout, *stderr;
#define EOF (-1)
#define BUFSIZ 1024
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2
#define FILENAME_MAX 512
#define L_tmpnam 24
#define TMP_MAX 100000
FILE *fopen(const char *path, const char *mode);
FILE *fdopen(int fd, const char *mode);
FILE *freopen(const char *path, const char *mode, FILE *f);   /* path must be non-NULL */
FILE *tmpfile(void);                                          /* deleted on fclose (not on crash) */
char *tmpnam(char *buf);                                      /* buf of L_tmpnam bytes, or NULL (static) */
int fclose(FILE *f);
size_t fread(void *p, size_t sz, size_t n, FILE *f);
size_t fwrite(const void *p, size_t sz, size_t n, FILE *f);
int fgetc(FILE *f); int getc(FILE *f); int getchar(void);
int ungetc(int c, FILE *f);
char *fgets(char *s, int n, FILE *f);
int fputc(int c, FILE *f); int putc(int c, FILE *f); int putchar(int c);
int fputs(const char *s, FILE *f); int puts(const char *s);
int fflush(FILE *f);
int fseek(FILE *f, long off, int whence);
long ftell(FILE *f);
void rewind(FILE *f);
int feof(FILE *f); int ferror(FILE *f); void clearerr(FILE *f);
int fileno(FILE *f);
int setvbuf(FILE *f, char *buf, int mode, size_t size);
void setbuf(FILE *f, char *buf);
void perror(const char *s);
int remove(const char *path);
int rename(const char *from, const char *to);
ssize_t getline(char **line, size_t *n, FILE *f);
ssize_t getdelim(char **line, size_t *n, int delim, FILE *f);
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int fprintf(FILE *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int sprintf(char *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int snprintf(char *s, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int asprintf(char **out, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int vprintf(const char *fmt, va_list ap);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int vsprintf(char *s, const char *fmt, va_list ap);
int vsnprintf(char *s, size_t n, const char *fmt, va_list ap);
int vasprintf(char **out, const char *fmt, va_list ap);
int scanf(const char *fmt, ...);
int fscanf(FILE *f, const char *fmt, ...);
int sscanf(const char *s, const char *fmt, ...);
int vsscanf(const char *s, const char *fmt, va_list ap);
int vfscanf(FILE *f, const char *fmt, va_list ap);
