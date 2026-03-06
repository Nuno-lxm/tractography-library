#define _GNU_SOURCE

#include <stdio.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>

#define LOG_FILE "logger.log"

// Function Pointer Types for Original System Calls
typedef int (*original_open_t)(const char *path, int flags, ...);
typedef ssize_t (*original_write_t)(int fd, const void *buf, size_t count);
typedef ssize_t (*original_read_t)(int fd, void *buf, size_t count);
typedef int (*original_close_t)(int fd);
typedef off_t (*original_lseek_t)(int fd, off_t offset, int whence);
typedef int (*original_openat_t)(int dirfd, const char *path, int flags, ...);
typedef ssize_t (*original_pread64_t)(int fd, void *buf, size_t count, off_t offset);

static __thread int in_log = 0;

static void log_msg(const char *fmt, ...) {
    static int log_fd = -1;
    if (in_log) return;
    in_log = 1;

    if (log_fd == -1) {
        log_fd = syscall(SYS_open, LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (log_fd < 0) {
            in_log = 0;
            return;
        }
    }

    char msg[512];
    char timestamp[64];
    char logbuf[576];

    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    strftime(timestamp, sizeof(timestamp), "[%Y-%m-%d %H:%M:%S] ", &tm_info);

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    if (n > 0) {
        int total = snprintf(logbuf, sizeof(logbuf), "%s%s", timestamp, msg);
        if (total > 0) {
            size_t len = (size_t)total < sizeof(logbuf) ? (size_t)total : sizeof(logbuf) - 1;
            syscall(SYS_write, log_fd, logbuf, len);
        }
    }

    in_log = 0;
}


int open(const char *path, int flags, ...) {
    static original_open_t original_open = NULL;
    if (!original_open) original_open = (original_open_t)dlsym(RTLD_NEXT, "open");

    va_list args;
    va_start(args, flags);
    int fd;

    if (flags & O_CREAT) {
        mode_t mode = va_arg(args, mode_t);
        fd = original_open(path, flags, mode);
    } else {
        fd = original_open(path, flags);
    }
    va_end(args);

    log_msg("[pid=%d] open('%s', flags=0x%x) = %d\n", getpid(), path, flags, fd);
    return fd;
}


ssize_t write(int fd, const void *buf, size_t count) {
    static original_write_t original_write = NULL;
    if (!original_write) original_write = (original_write_t)dlsym(RTLD_NEXT, "write");

    ssize_t bytes = original_write(fd, buf, count);
    log_msg("[pid=%d] write(fd=%d, count=%zu) = %zd\n", getpid(), fd, count, bytes);
    return bytes;
}




ssize_t read(int fd, void *buf, size_t count) {
    static original_read_t original_read = NULL;
    if (!original_read) original_read = (original_read_t)dlsym(RTLD_NEXT, "read");

    ssize_t bytes = original_read(fd, buf, count);
    log_msg("[pid=%d] read(fd=%d, count=%zu) = %zd\n", getpid(), fd, count, bytes);
    return bytes;
}



int close(int fd) {
    static original_close_t original_close = NULL;
    if (!original_close) original_close = (original_close_t)dlsym(RTLD_NEXT, "close");

    int result = original_close(fd);
    log_msg("[pid=%d] close(fd=%d) = %d\n", getpid(), fd, result);
    return result;
}


off_t lseek(int fd, off_t offset, int whence) {
    static original_lseek_t original_lseek = NULL;
    if (!original_lseek) original_lseek = (original_lseek_t)dlsym(RTLD_NEXT, "lseek");

    off_t result = original_lseek(fd, offset, whence);

    const char *whence_str = (whence == SEEK_SET) ? "SEEK_SET" :
                             (whence == SEEK_CUR) ? "SEEK_CUR" :
                             (whence == SEEK_END) ? "SEEK_END" : "UNKNOWN";

    log_msg("[pid=%d] lseek(fd=%d, offset=%ld, whence=%s) = %ld\n",
            getpid(), fd, (long)offset, whence_str, (long)result);

    return result;
}


int openat(int dirfd, const char *path, int flags, ...) {
    static original_openat_t original_openat = NULL;
    if (!original_openat) original_openat = (original_openat_t)dlsym(RTLD_NEXT, "openat");

    va_list args;
    va_start(args, flags);
    int fd;

    if (flags & O_CREAT) {
        mode_t mode = va_arg(args, mode_t);
        fd = original_openat(dirfd, path, flags, mode);
    } else {
        fd = original_openat(dirfd, path, flags);
    }
    va_end(args);

    log_msg("[pid=%d] openat(dirfd=%d, path='%s', flags=0x%x) = %d\n", getpid(), dirfd, path, flags, fd);
    return fd;
}


ssize_t pread64(int fd, void *buf, size_t count, off_t offset) {
    static original_pread64_t original_pread64 = NULL;
    if (!original_pread64) original_pread64 = (original_pread64_t)dlsym(RTLD_NEXT, "pread64");

    ssize_t bytes = original_pread64(fd, buf, count, offset);
    log_msg("[pid=%d] pread64(fd=%d, count=%zu, offset=%ld) = %zd\n", getpid(), fd, count, (long)offset, bytes);
    return bytes;
}