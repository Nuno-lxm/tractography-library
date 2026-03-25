#define _GNU_SOURCE

#include <stdio.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <sys/stat.h>


#define LOG_FILE "/mnt/logger.log"

static char cmdline[512];

// Function Pointer Types for Original System Calls
typedef int (*original_open_t)(const char *path, int flags, ...);
typedef ssize_t (*original_write_t)(int fd, const void *buf, size_t count);
typedef ssize_t (*original_read_t)(int fd, void *buf, size_t count);
typedef int (*original_close_t)(int fd);
typedef off_t (*original_lseek_t)(int fd, off_t offset, int whence);
typedef int (*original_openat_t)(int dirfd, const char *path, int flags, ...);
typedef ssize_t (*original_pread64_t)(int fd, void *buf, size_t count, off_t offset);
typedef int (*original_stat_t)(const char *path, struct stat *buf);
typedef int (*original_fstat_t)(int fd, struct stat *buf);
typedef pid_t (*original_fork_t)(void);
typedef int (*original_dup_t)(int fd);
typedef int (*original_dup2_t)(int oldfd, int newfd);

static __thread int in_log = 0;
static int log_fd = -1;

static void log_msg(const char *fmt, ...) {
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
    char logbuf[704];

    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    strftime(timestamp, sizeof(timestamp), "[%Y-%m-%d %H:%M:%S] ", &tm_info);

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    if (n > 0) {
        int total = snprintf(
            logbuf,
            sizeof(logbuf),
            "%s[pid=%ld] %s",
            timestamp,
            (long)getpid(),
            msg
        );
        if (total > 0) {
            size_t len = (size_t)total < sizeof(logbuf) ? (size_t)total : sizeof(logbuf) - 1;
            syscall(SYS_write, log_fd, logbuf, len);
        }
    }

    in_log = 0;
}

static int get_cmdline(char *out, size_t out_size) {
    if (!out || out_size < 2) return -1;

    char path[64];
    pid_t pid = getpid();
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);

    int fd = syscall(SYS_open, path, O_RDONLY);
    if (fd < 0) return -1;

    ssize_t bytes = syscall(SYS_read, fd, out, out_size - 1);
    syscall(SYS_close, fd);

    if (bytes > 0) {
        out[bytes] = '\0';
        for (ssize_t i = 0; i < bytes; i++) {
            if (out[i] == '\0') {
                out[i] = ' ';
            }
        }
    }
    return 0;
}

__attribute__((constructor)) static void logger_init() {
    get_cmdline(cmdline, sizeof(cmdline));
    log_msg("Initializing logger library...\n");
    log_msg("%s\n", cmdline);
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

    log_msg("open('%s', flags=0x%x) = %d\n", path, flags, fd);
    return fd;
}


ssize_t write(int fd, const void *buf, size_t count) {
    static original_write_t original_write = NULL;
    if (!original_write) original_write = (original_write_t)dlsym(RTLD_NEXT, "write");

    ssize_t bytes = original_write(fd, buf, count);
    log_msg("write(fd=%d, count=%zu) = %zd\n", fd, count, bytes);
    return bytes;
}




ssize_t read(int fd, void *buf, size_t count) {
    static original_read_t original_read = NULL;
    if (!original_read) original_read = (original_read_t)dlsym(RTLD_NEXT, "read");

    ssize_t bytes = original_read(fd, buf, count);
    log_msg("read(fd=%d, count=%zu) = %zd\n", fd, count, bytes);
    return bytes;
}



int close(int fd) {
    static original_close_t original_close = NULL;
    if (!original_close) original_close = (original_close_t)dlsym(RTLD_NEXT, "close");

    int result = original_close(fd);
    log_msg("close(fd=%d) = %d\n", fd, result);
    return result;
}


off_t lseek(int fd, off_t offset, int whence) {
    static original_lseek_t original_lseek = NULL;
    if (!original_lseek) original_lseek = (original_lseek_t)dlsym(RTLD_NEXT, "lseek");

    off_t result = original_lseek(fd, offset, whence);

    const char *whence_str = (whence == SEEK_SET) ? "SEEK_SET" :
                             (whence == SEEK_CUR) ? "SEEK_CUR" :
                             (whence == SEEK_END) ? "SEEK_END" : "UNKNOWN";

    log_msg("lseek(fd=%d, offset=%ld, whence=%s) = %ld\n",
            fd, (long)offset, whence_str, (long)result);

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

    log_msg("openat(dirfd=%d, path='%s', flags=0x%x) = %d\n", dirfd, path, flags, fd);
    return fd;
}


ssize_t pread64(int fd, void *buf, size_t count, off_t offset) {
    static original_pread64_t original_pread64 = NULL;
    if (!original_pread64) original_pread64 = (original_pread64_t)dlsym(RTLD_NEXT, "pread64");

    ssize_t bytes = original_pread64(fd, buf, count, offset);
    log_msg("pread64(fd=%d, count=%zu, offset=%ld) = %zd\n", fd, count, (long)offset, bytes);
    return bytes;
}


int stat(const char *path, struct stat *buf) {
    static original_stat_t original_stat = NULL;
    if (!original_stat) original_stat = (original_stat_t)dlsym(RTLD_NEXT, "stat");

    int result = original_stat(path, buf);
    log_msg("stat(path='%s') = %d\n", path, result);
    return result;
}


int fstat(int fd, struct stat *buf) {
    static original_fstat_t original_fstat = NULL;
    if (!original_fstat) original_fstat = (original_fstat_t)dlsym(RTLD_NEXT, "fstat");

    int result = original_fstat(fd, buf);
    log_msg("fstat(fd=%d) = %d\n", fd, result);
    return result;
}


pid_t fork(void) {
    static original_fork_t original_fork = NULL;
    if (!original_fork) original_fork = (original_fork_t)dlsym(RTLD_NEXT, "fork");

    pid_t pid = original_fork();
    log_msg("fork() = %d\n", pid);
    return pid;
}


int dup(int fd) {
    static original_dup_t original_dup = NULL;
    if (!original_dup) original_dup = (original_dup_t)dlsym(RTLD_NEXT, "dup");

    int newfd = original_dup(fd);
    log_msg("dup(fd=%d) = %d\n", fd, newfd);
    return newfd;
}


int dup2(int oldfd, int newfd) {
    static original_dup2_t original_dup2 = NULL;
    if (!original_dup2) original_dup2 = (original_dup2_t)dlsym(RTLD_NEXT, "dup2");

    int result = original_dup2(oldfd, newfd);
    log_msg("dup2(oldfd=%d, newfd=%d) = %d\n", oldfd, newfd, result);
    return result;
}