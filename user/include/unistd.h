#pragma once
#include "abi.h"
#include <stddef.h>
#include <sys/types.h>
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
extern char **environ;
ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
int close(int fd);
off_t lseek(int fd, off_t off, int whence);
int unlink(const char *path);
int rmdir(const char *path);
int chdir(const char *path);
char *getcwd(char *buf, size_t n);
pid_t getpid(void);
pid_t getppid(void);
void *sbrk(int incr);
int dup(int fd);
int dup2(int old, int new_);
int pipe(int fds[2]);
int isatty(int fd);
int ftruncate(int fd, off_t len);
unsigned sleep(unsigned sec);
int usleep(unsigned us);
void _exit(int code) __attribute__((noreturn));
/* spawn: path to executable, argv NULL-terminated; returns child pid or -1 */
int spawnv(const char *path, char *const argv[]);
int spawnve(const char *path, char *const argv[], char *const envp[], const struct spawn_opts *o);
int spawnvp(const char *file, char *const argv[]);          /* searches $PATH (default "/bin") */
int execvp(const char *file, char *const argv[]);          /* emulated: spawn + wait + _exit (no exec in the kernel) */
pid_t waitpid(pid_t pid, int *status, int flags);
pid_t wait(int *status);
