#include <errno.h>
#include <string.h>

int errno;

static const struct { int e; const char *s; } errs[] = {
    { EPERM, "Operation not permitted" }, { ENOENT, "No such file or directory" },
    { ESRCH, "No such process" }, { EINTR, "Interrupted" }, { EIO, "I/O error" },
    { ENXIO, "No such device or address" }, { E2BIG, "Argument list too long" },
    { ENOEXEC, "Exec format error" }, { EBADF, "Bad file descriptor" },
    { ECHILD, "No child processes" }, { EAGAIN, "Try again" }, { ENOMEM, "Out of memory" },
    { EACCES, "Permission denied" }, { EFAULT, "Bad address" }, { EBUSY, "Device or resource busy" },
    { EEXIST, "File exists" }, { EXDEV, "Cross-device link" }, { ENODEV, "No such device" },
    { ENOTDIR, "Not a directory" }, { EISDIR, "Is a directory" }, { EINVAL, "Invalid argument" },
    { ENFILE, "File table overflow" }, { EMFILE, "Too many open files" },
    { ENOTTY, "Not a tty" }, { EFBIG, "File too large" }, { ENOSPC, "No space left on device" },
    { ESPIPE, "Illegal seek" }, { EROFS, "Read-only file system" }, { EPIPE, "Broken pipe" },
    { ERANGE, "Result too large" }, { ENAMETOOLONG, "File name too long" },
    { ENOSYS, "Function not implemented" }, { ENOTEMPTY, "Directory not empty" },
};

char *strerror(int e) {
    for (unsigned i = 0; i < sizeof errs / sizeof *errs; i++)
        if (errs[i].e == e) return (char *)errs[i].s;
    return "Unknown error";
}
