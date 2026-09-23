#define _GNU_SOURCE

#include <stdio.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <errno.h>
#include <fnmatch.h>

/*
Configuration
*/

#define INITIAL_FD_TABLE_SIZE 64

#define LOG_FILE "/mnt/buffer.log"

#define DEFAULT_ENABLE_WRITE_BUFFERING 1
#define DEFAULT_ENABLE_READ_PREFETCH 1
#define DEFAULT_WRITE_BUFFER_SIZE (256 * 1024)
#define DEFAULT_PREFETCH_BUFFER_SIZE (256 * 1024)

#ifndef BUFFER_LIB_VERSION
#define BUFFER_LIB_VERSION "unknown"
#endif

static char *prefetching_targets[] = {
    NULL
};

static char *buffering_targets[] = {
    NULL
};

static char *log_file_path = LOG_FILE;
static char **prefetch_target_list = prefetching_targets;
static char **buffer_target_list = buffering_targets;
static char **env_prefetch_targets = NULL;
static char **env_buffer_targets = NULL;
static char **env_file_targets = NULL;
static char **file_target_list = NULL;

static int enable_write_buffering = DEFAULT_ENABLE_WRITE_BUFFERING;
static int enable_read_prefetch = DEFAULT_ENABLE_READ_PREFETCH;
static size_t write_buffer_size = DEFAULT_WRITE_BUFFER_SIZE;
static size_t prefetch_buffer_size = DEFAULT_PREFETCH_BUFFER_SIZE;

/*
Function Pointer Types for Original System Calls
*/

typedef int (*original_open_t)(const char *path, int flags, ...);
typedef int (*original_open64_t)(const char *path, int flags, ...);
typedef int (*original_openat_t)(int dirfd, const char *path, int flags, ...);
typedef int (*original_openat64_t)(int dirfd, const char *path, int flags, ...);
typedef ssize_t (*original_write_t)(int fd, const void *buf, size_t count);
typedef ssize_t (*original_pwrite_t)(int fd, const void *buf, size_t count, off_t offset);
typedef ssize_t (*original_read_t)(int fd, void *buf, size_t count);
typedef int (*original_close_t)(int fd);
typedef off_t (*original_lseek_t)(int fd, off_t offset, int whence);
typedef off64_t (*original_lseek64_t)(int fd, off64_t offset, int whence);
typedef int (*original_fsync_t)(int fd);
typedef int (*original_fdatasync_t)(int fd);
typedef int (*original_dup2_t)(int oldfd, int newfd);
typedef int (*original_dup3_t)(int oldfd, int newfd, int flags);
typedef pid_t (*original_fork_t)(void);

/*
Define Buffer array
*/

typedef struct {
    int fd;
    off_t logical_offset;
    off_t kernel_offset;
    int seekable;
    int buffer_enabled;
    int prefetch_enabled;
    int filter_initialized;
    int append;
} fd_state_t;

typedef struct {
    fd_state_t state;
    char *wr_buf;
    size_t wr_pos;
    char *pf_buf;
    size_t pf_pos;
    size_t pf_data_size;
} fd_entry_t;

static fd_entry_t *fd_table = NULL;
static size_t fd_table_cap = 0;

/*
Statistics Variables
*/

static unsigned long intercept_writes = 0;
static unsigned long syscall_writes = 0;

static size_t intercept_wr_bytes = 0;
static size_t syscall_wr_bytes = 0;

static unsigned long intercept_reads = 0;
static unsigned long syscall_reads = 0;

static size_t intercept_rd_bytes = 0;
static size_t syscall_rd_bytes = 0;

// Calls on fds the library does not buffer; kept out of the counts above
static unsigned long passthrough_writes = 0;
static size_t passthrough_wr_bytes = 0;
static unsigned long passthrough_reads = 0;
static size_t passthrough_rd_bytes = 0;

static unsigned long flush_on_reads = 0;

/*
Forward Declarations
*/

static void log_msg(const char *fmt, ...);
static int get_process_name(char *out, size_t out_size);
static int get_script_name(char *out, size_t out_size);
static int target_list_matches_process_name(char **target_list);
static void init_config_from_env(void);
static char **split_target_list(const char *value);
static void free_target_list(char **list);
static const char *path_basename(const char *path);
static int list_matches_path(char **patterns, const char *path, const char *base);
static void configure_fd_state_for_path(int fd, fd_state_t *state, const char *path, int flags);

static char process_name[256];
static char script_name[256];
static int buffering_target = 0;
static int prefetching_target = 0;

__attribute__((constructor)) static void init_buffers();
__attribute__((destructor)) static void cleanup_buffers();

/*
Initialization
*/

static void init_buffers() {
    get_process_name(process_name, sizeof(process_name));
    if (fnmatch("*python*", process_name, 0) == 0) {
        get_script_name(script_name, sizeof(script_name));
    }
    init_config_from_env();

    if (enable_write_buffering && target_list_matches_process_name(buffer_target_list)) buffering_target = 1;
    if (enable_read_prefetch && target_list_matches_process_name(prefetch_target_list)) prefetching_target = 1;

    if (!buffering_target && !prefetching_target) return;

    log_msg("Initializing buffer library (version %s)...\n%s%s%s\nBuffering: %d; Prefetching: %d\n",
        BUFFER_LIB_VERSION,
        process_name[0] ? process_name : "unknown",
        script_name[0] ? " " : "",
        script_name,
        buffering_target,
        prefetching_target);
}

/*
Utility Functions
*/

static __thread int in_log = 0;
static int log_fd = -1;

static void log_msg(const char *fmt, ...) {
    if (in_log) return;
    in_log = 1;

    if (log_fd == -1) {
        const char *path = log_file_path ? log_file_path : LOG_FILE;
        log_fd = syscall(SYS_open, path, O_WRONLY | O_CREAT | O_APPEND, 0644);
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

static int get_process_name(char *out, size_t out_size) {
    if (!out || out_size < 2) return -1;

    out[0] = '\0';
    char exe_path[1024];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len <= 0) return -1;

    exe_path[(size_t)len] = '\0';

    const char *base = strrchr(exe_path, '/');
    base = base ? base + 1 : exe_path;

    if (!*base) return -1;

    snprintf(out, out_size, "%s", base);
    return 0;
}

/*
For interpreters the exe is e.g. python3.7, so also read the script from
/proc/self/cmdline: the first argument that is not an option (or the module
after -m). Uses raw syscalls so the reads are not counted in the stats.
*/
static int get_script_name(char *out, size_t out_size) {
    if (!out || out_size < 2) return -1;

    out[0] = '\0';
    char cmdline[4096];
    int fd = syscall(SYS_open, "/proc/self/cmdline", O_RDONLY);
    if (fd < 0) return -1;
    ssize_t len = syscall(SYS_read, fd, cmdline, sizeof(cmdline) - 1);
    syscall(SYS_close, fd);
    if (len <= 0) return -1;
    cmdline[len] = '\0';

    // Arguments are NUL-separated; skip argv[0]
    const char *arg = cmdline + strlen(cmdline) + 1;
    const char *end = cmdline + len;
    while (arg < end) {
        if (strcmp(arg, "-c") == 0) return -1;
        int takes_value = strcmp(arg, "-m") == 0 || strcmp(arg, "-W") == 0 || strcmp(arg, "-X") == 0;
        if (arg[0] != '-') {
            snprintf(out, out_size, "%s", path_basename(arg));
            return 0;
        }
        const char *next = arg + strlen(arg) + 1;
        if (takes_value && strcmp(arg, "-m") == 0 && next < end) {
            snprintf(out, out_size, "%s", next);
            return 0;
        }
        arg = takes_value && next < end ? next + strlen(next) + 1 : next;
    }

    return -1;
}

static int target_list_matches_process_name(char **target_list) {
    if (!target_list || !process_name[0]) return 0;

    for (char **t = target_list; *t; t++) {
        if (strcmp(process_name, *t) == 0) {
            return 1;
        }
        if (script_name[0] && strcmp(script_name, *t) == 0) {
            return 1;
        }
    }

    return 0;
}

static int is_target_delim(char c) {
    return c == ',' || c == ' ' || c == '\t' || c == '\n' || c == ';';
}

static char **split_target_list(const char *value) {
    if (!value) return NULL;

    while (*value && is_target_delim(*value)) value++;
    if (*value == '\0') return NULL;

    size_t count = 0;
    const char *p = value;
    while (*p) {
        while (*p && is_target_delim(*p)) p++;
        if (!*p) break;
        count++;
        while (*p && !is_target_delim(*p)) p++;
    }

    if (count == 0) return NULL;

    char **list = calloc(count + 1, sizeof(*list));
    if (!list) return NULL;

    size_t idx = 0;
    p = value;
    while (*p && idx < count) {
        while (*p && is_target_delim(*p)) p++;
        if (!*p) break;
        const char *start = p;
        while (*p && !is_target_delim(*p)) p++;
        size_t len = (size_t)(p - start);
        char *token = strndup(start, len);
        if (!token) {
            free_target_list(list);
            return NULL;
        }
        list[idx++] = token;
    }

    list[idx] = NULL;
    return list;
}

static void free_target_list(char **list) {
    if (!list) return;
    for (size_t i = 0; list[i]; i++) {
        free(list[i]);
    }
    free(list);
}

static void init_config_from_env(void) {
    const char *log_env = getenv("BUFFER_LOG_FILE");
    if (log_env && *log_env) {
        log_file_path = (char *)log_env;
    }

    const char *prefetch_env = getenv("BUFFER_PREFETCH_TARGETS");
    if (prefetch_env) {
        char **list = split_target_list(prefetch_env);
        if (list || *prefetch_env == '\0') {
            env_prefetch_targets = list;
            prefetch_target_list = list;
        } else {
            log_msg("Failed to parse BUFFER_PREFETCH_TARGETS\n");
        }
    }

    const char *buffer_env = getenv("BUFFER_WRITE_TARGETS");
    if (buffer_env) {
        char **list = split_target_list(buffer_env);
        if (list || *buffer_env == '\0') {
            env_buffer_targets = list;
            buffer_target_list = list;
        } else {
            log_msg("Failed to parse BUFFER_WRITE_TARGETS\n");
        }
    }

    const char *file_env = getenv("BUFFER_FILE_TARGETS");
    if (file_env && *file_env) {
        char **list = split_target_list(file_env);
        if (list) {
            env_file_targets = list;
            file_target_list = list;
        } else {
            log_msg("Failed to parse BUFFER_FILE_TARGETS\n");
        }
    }

    const char *wb_env = getenv("BUFFER_WRITE_ENABLED");
    if (wb_env && *wb_env) {
        enable_write_buffering = atoi(wb_env);
    }

    const char *rp_env = getenv("BUFFER_PREFETCH_ENABLED");
    if (rp_env && *rp_env) {
        enable_read_prefetch = atoi(rp_env);
    }

    const char *ws_env = getenv("BUFFER_WRITE_SIZE");
    if (ws_env && *ws_env) {
        unsigned long val = strtoul(ws_env, NULL, 10);
        if (val > 0) write_buffer_size = (size_t)val;
    }

    const char *ps_env = getenv("BUFFER_PREFETCH_SIZE");
    if (ps_env && *ps_env) {
        unsigned long val = strtoul(ps_env, NULL, 10);
        if (val > 0) prefetch_buffer_size = (size_t)val;
    }
}

static const char *path_basename(const char *path) {
    if (!path) return NULL;
    const char *base = strrchr(path, '/');
    return base ? base + 1 : path;
}

static int list_matches_path(char **patterns, const char *path, const char *base) {
    if (!patterns || !path || !base) return 0;
    for (char **p = patterns; *p; p++) {
        const char *target = strchr(*p, '/') ? path : base;
        if (fnmatch(*p, target, 0) == 0) return 1;
    }
    return 0;
}

static void configure_fd_state_for_path(int fd, fd_state_t *state, const char *path, int flags) {
    if (!state || state->filter_initialized) return;

    state->buffer_enabled = buffering_target;
    state->prefetch_enabled = prefetching_target;

    if (file_target_list) {
        if (path && *path) {
            const char *base = path_basename(path);
            int match = list_matches_path(file_target_list, path, base);
            state->buffer_enabled = match ? buffering_target : 0;
            state->prefetch_enabled = match ? prefetching_target : 0;
        } else {
            state->buffer_enabled = 0;
            state->prefetch_enabled = 0;
        }
    }

    // Only buffer writes on fds open for writing, only prefetch on fds open for reading
    int accmode = flags & O_ACCMODE;
    if (accmode == O_RDONLY) state->buffer_enabled = 0;
    if (accmode == O_WRONLY) state->prefetch_enabled = 0;

    state->append = (flags & O_APPEND) != 0;
    state->filter_initialized = 1;
}

/*
Statistics Functions
*/

static inline void update_intercept_writes(size_t count) {
    intercept_writes++;
    intercept_wr_bytes += count;
}

static inline void update_syscall_writes(size_t count) {
    syscall_writes++;
    syscall_wr_bytes += count;
}

static inline void update_intercept_reads(size_t count) {
    intercept_reads++;
    intercept_rd_bytes += count;
}

static inline void update_syscall_reads(size_t count) {
    syscall_reads++;
    syscall_rd_bytes += count;
}

static inline void update_passthrough_writes(size_t count) {
    passthrough_writes++;
    passthrough_wr_bytes += count;
}

static inline void update_passthrough_reads(size_t count) {
    passthrough_reads++;
    passthrough_rd_bytes += count;
}

/*
FD Table Management
*/

static int ensure_fd_table(int fd) {
    if (fd < 0) return 0;

    if ((size_t)fd < fd_table_cap) return 1;

    size_t new_cap = fd_table_cap ? fd_table_cap : INITIAL_FD_TABLE_SIZE;
    while (new_cap <= (size_t)fd) new_cap *= 2;

    fd_entry_t *new_table = realloc(fd_table, new_cap * sizeof(*new_table));
    if (!new_table) return 0;

    memset(new_table + fd_table_cap, 0, (new_cap - fd_table_cap) * sizeof(*new_table));
    for (size_t i = fd_table_cap; i < new_cap; i++) {
        new_table[i].state.fd = -1;
    }

    fd_table = new_table;
    fd_table_cap = new_cap;
    return 1;
}

static fd_entry_t *get_fd_entry(int fd) {
    if (fd < 0) return NULL;
    if ((size_t)fd >= fd_table_cap) return NULL;
    fd_entry_t *entry = &fd_table[fd];
    if (entry->state.fd < 0) return NULL;
    return entry;
}

static fd_entry_t *get_or_create_fd_entry(int fd) {
    if (fd < 2) return NULL;
    if (!ensure_fd_table(fd)) return NULL;

    fd_entry_t *entry = &fd_table[fd];
    if (entry->state.fd >= 0) return entry;

    entry->state.fd = fd;
    entry->state.logical_offset = 0;
    entry->state.kernel_offset = 0;
    entry->state.seekable = 1;
    entry->state.buffer_enabled = 0;
    entry->state.prefetch_enabled = 0;
    entry->state.filter_initialized = 0;
    entry->state.append = 0;

    return entry;
}

static void free_fd_entry(int fd) {
    if (fd < 0 || (size_t)fd >= fd_table_cap) return;
    fd_entry_t *entry = &fd_table[fd];
    if (entry->state.fd < 0) return;

    free(entry->wr_buf);
    free(entry->pf_buf);
    memset(entry, 0, sizeof(*entry));
    entry->state.fd = -1;
}

/*
An fd returned by open() can still have an entry if the previous file with
that number was closed without going through close() (e.g. fclose after
fdopen). Drop it so the new file does not inherit its buffers or offsets.
*/
static void drop_stale_fd_entry(int fd) {
    fd_entry_t *entry = get_fd_entry(fd);
    if (!entry) return;

    if (entry->wr_pos > 0) {
        log_msg("Dropping stale entry for fd %d with %zu unflushed bytes\n", fd, entry->wr_pos);
    }
    free_fd_entry(fd);
}

static void clear_all_fd_entries(void) {
    for (size_t i = 0; i < fd_table_cap; i++) {
        fd_entry_t *entry = &fd_table[i];
        if (entry->state.fd >= 0) {
            free(entry->wr_buf);
            free(entry->pf_buf);
            memset(entry, 0, sizeof(*entry));
            entry->state.fd = -1;
        }
    }
}

/*
Offset helpers
*/

static void advance_kernel_offset(fd_state_t *state, ssize_t delta) {
    if (!state || delta <= 0) return;
    state->kernel_offset += (off_t)delta;
}

static void advance_logical_offset(fd_state_t *state, ssize_t delta) {
    if (!state || delta <= 0) return;
    state->logical_offset += (off_t)delta;
}

static int sync_kernel_to_logical(int fd, fd_state_t *state) {
    if (!state) return 0;
    if (state->seekable == 0) return 0;
    if (state->kernel_offset == state->logical_offset) return 0;

    off_t result = syscall(SYS_lseek, fd, state->logical_offset, SEEK_SET);
    if (result < 0) {
        if (errno == ESPIPE) {
            state->seekable = 0;
            return 0;
        }
        return -1;
    }

    state->kernel_offset = result;
    state->logical_offset = result;
    return 0;
}

/*
Write buffer management
*/

static int flush_write_buffer(int fd, char *buf, size_t *pos, fd_state_t *state) {
    static original_write_t original_write = NULL;
    if (!original_write) original_write = dlsym(RTLD_NEXT, "write");

    if (*pos == 0 || fd < 0) return 0;

    size_t remaining = *pos;
    size_t offset = 0;

    while (remaining > 0) {
        ssize_t bytes = original_write(fd, buf + offset, remaining);
        if (bytes < 0) return -1;
        if (bytes == 0) {
            errno = EIO;
            return -1;
        }
        offset += (size_t)bytes;
        remaining -= (size_t)bytes;
        update_syscall_writes((size_t)bytes);
        advance_kernel_offset(state, bytes);
    }

    *pos = 0;
    return 0;
}

/*
Prefetching leaves the kernel offset ahead of the logical offset. Drop the
prefetched data and seek back so a following write lands in the right place.
*/

static int discard_prefetch(int fd, fd_entry_t *entry) {
    if (!entry->pf_buf || entry->pf_data_size == 0) return 0;

    if (sync_kernel_to_logical(fd, &entry->state) < 0) return -1;
    entry->pf_pos = 0;
    entry->pf_data_size = 0;
    return 0;
}

/*
Flush both write and prefetch buffers for an fd
*/

static int flush_buffers(int fd) {
    fd_entry_t *entry = get_fd_entry(fd);
    if (!entry) return 0;

    if (entry->state.buffer_enabled && entry->wr_buf && entry->wr_pos > 0) {
        if (flush_write_buffer(fd, entry->wr_buf, &entry->wr_pos, &entry->state) < 0) {
            return -1;
        }
    }

    if (entry->pf_buf) {
        if (sync_kernel_to_logical(fd, &entry->state) < 0) {
            return -1;
        }
        entry->pf_pos = 0;
        entry->pf_data_size = 0;
    }

    return 0;
}

/*
Intercepted Functions
*/

// File opening and closing

int open(const char *path, int flags, ...) {
    static original_open_t original_open = NULL;
    if (!original_open) original_open = dlsym(RTLD_NEXT, "open");

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

    if (fd >= 0 && (buffering_target || prefetching_target)) {
        drop_stale_fd_entry(fd);
        fd_entry_t *entry = get_or_create_fd_entry(fd);
        configure_fd_state_for_path(fd, entry ? &entry->state : NULL, path, flags);
        if (entry && !entry->state.buffer_enabled && !entry->state.prefetch_enabled) {
            free_fd_entry(fd);
        }
    }

    return fd;
}

int open64(const char *path, int flags, ...) {
    static original_open64_t original_open64 = NULL;
    if (!original_open64) original_open64 = dlsym(RTLD_NEXT, "open64");

    va_list args;
    va_start(args, flags);
    int fd;

    if (original_open64) {
        if (flags & O_CREAT) {
            mode_t mode = va_arg(args, mode_t);
            fd = original_open64(path, flags, mode);
        } else {
            fd = original_open64(path, flags);
        }
    } else {
        static original_open_t original_open = NULL;
        if (!original_open) original_open = dlsym(RTLD_NEXT, "open");
        if (flags & O_CREAT) {
            mode_t mode = va_arg(args, mode_t);
            fd = original_open(path, flags, mode);
        } else {
            fd = original_open(path, flags);
        }
    }

    va_end(args);

    if (fd >= 0 && (buffering_target || prefetching_target)) {
        drop_stale_fd_entry(fd);
        fd_entry_t *entry = get_or_create_fd_entry(fd);
        configure_fd_state_for_path(fd, entry ? &entry->state : NULL, path, flags);
        if (entry && !entry->state.buffer_enabled && !entry->state.prefetch_enabled) {
            free_fd_entry(fd);
        }
    }

    return fd;
}

int openat(int dirfd, const char *path, int flags, ...) {
    static original_openat_t original_openat = NULL;
    if (!original_openat) original_openat = dlsym(RTLD_NEXT, "openat");

    va_list args;
    va_start(args, flags);
    int fd;

    if (original_openat) {
        if (flags & O_CREAT) {
            mode_t mode = va_arg(args, mode_t);
            fd = original_openat(dirfd, path, flags, mode);
        } else {
            fd = original_openat(dirfd, path, flags);
        }
    } else {
        if (flags & O_CREAT) {
            mode_t mode = va_arg(args, mode_t);
            fd = syscall(SYS_openat, dirfd, path, flags, mode);
        } else {
            fd = syscall(SYS_openat, dirfd, path, flags);
        }
    }

    va_end(args);

    if (fd >= 0 && (buffering_target || prefetching_target)) {
        drop_stale_fd_entry(fd);
        fd_entry_t *entry = get_or_create_fd_entry(fd);
        configure_fd_state_for_path(fd, entry ? &entry->state : NULL, path, flags);
        if (entry && !entry->state.buffer_enabled && !entry->state.prefetch_enabled) {
            free_fd_entry(fd);
        }
    }

    return fd;
}

int openat64(int dirfd, const char *path, int flags, ...) {
    static original_openat64_t original_openat64 = NULL;
    if (!original_openat64) original_openat64 = dlsym(RTLD_NEXT, "openat64");

    va_list args;
    va_start(args, flags);
    int fd;

    if (original_openat64) {
        if (flags & O_CREAT) {
            mode_t mode = va_arg(args, mode_t);
            fd = original_openat64(dirfd, path, flags, mode);
        } else {
            fd = original_openat64(dirfd, path, flags);
        }
    } else {
        static original_openat_t original_openat = NULL;
        if (!original_openat) original_openat = dlsym(RTLD_NEXT, "openat");
        if (original_openat) {
            if (flags & O_CREAT) {
                mode_t mode = va_arg(args, mode_t);
                fd = original_openat(dirfd, path, flags, mode);
            } else {
                fd = original_openat(dirfd, path, flags);
            }
        } else {
            if (flags & O_CREAT) {
                mode_t mode = va_arg(args, mode_t);
                fd = syscall(SYS_openat, dirfd, path, flags, mode);
            } else {
                fd = syscall(SYS_openat, dirfd, path, flags);
            }
        }
    }

    va_end(args);

    if (fd >= 0 && (buffering_target || prefetching_target)) {
        drop_stale_fd_entry(fd);
        fd_entry_t *entry = get_or_create_fd_entry(fd);
        configure_fd_state_for_path(fd, entry ? &entry->state : NULL, path, flags);
        if (entry && !entry->state.buffer_enabled && !entry->state.prefetch_enabled) {
            free_fd_entry(fd);
        }
    }

    return fd;
}

int close(int fd) {
    static original_close_t original_close = NULL;
    if (!original_close) original_close = dlsym(RTLD_NEXT, "close");

    int flush_errno = 0;

    fd_entry_t *entry = get_fd_entry(fd);
    if (entry) {
        if (entry->state.buffer_enabled && entry->wr_buf && entry->wr_pos > 0) {
            if (flush_write_buffer(fd, entry->wr_buf, &entry->wr_pos, &entry->state) < 0) {
                flush_errno = errno;
            }
        }
        free_fd_entry(fd);
    }

    int result = original_close(fd);
    if (flush_errno) {
        errno = flush_errno;
        return -1;
    }

    return result;
}

// Write operations

ssize_t write(int fd, const void *buf, size_t count) {
    static original_write_t original_write = NULL;
    if (!original_write) original_write = dlsym(RTLD_NEXT, "write");

    if (fd < 2) {
        return original_write(fd, buf, count);
    }

    fd_entry_t *entry = (buffering_target || prefetching_target)
        ? get_fd_entry(fd)
        : NULL;

    if (entry && discard_prefetch(fd, entry) < 0) return -1;

    if (!entry || !entry->state.buffer_enabled) {
        ssize_t bytes = original_write(fd, buf, count);
        update_passthrough_writes(bytes > 0 ? (size_t)bytes : 0);
        if (entry) {
            advance_kernel_offset(&entry->state, bytes);
            advance_logical_offset(&entry->state, bytes);
        }
        return bytes;
    }

    update_intercept_writes(count);

    if (!entry->wr_buf) {
        entry->wr_buf = malloc(write_buffer_size);
        if (!entry->wr_buf) {
            ssize_t bytes = original_write(fd, buf, count);
            update_syscall_writes(bytes > 0 ? (size_t)bytes : 0);
            advance_kernel_offset(&entry->state, bytes);
            advance_logical_offset(&entry->state, bytes);
            return bytes;
        }
        entry->wr_pos = 0;
    }

    if (entry->wr_pos + count > write_buffer_size) {
        if (flush_write_buffer(fd, entry->wr_buf, &entry->wr_pos, &entry->state) < 0) {
            return -1;
        }

        if (count > write_buffer_size) {
            ssize_t bytes = original_write(fd, buf, count);
            update_syscall_writes(bytes > 0 ? (size_t)bytes : 0);
            advance_kernel_offset(&entry->state, bytes);
            advance_logical_offset(&entry->state, bytes);
            return bytes;
        }
    }

    memcpy(entry->wr_buf + entry->wr_pos, buf, count);
    entry->wr_pos += count;
    advance_logical_offset(&entry->state, (ssize_t)count);

    return count;
}

ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset) {
    static original_pwrite_t original_pwrite = NULL;
    if (!original_pwrite) original_pwrite = dlsym(RTLD_NEXT, "pwrite");

    if (flush_buffers(fd) < 0) return -1;

    return original_pwrite(fd, buf, count, offset);
}

// Read operations

ssize_t read(int fd, void *buf, size_t count) {
    static original_read_t original_read = NULL;
    if (!original_read) original_read = dlsym(RTLD_NEXT, "read");

    fd_entry_t *entry = (buffering_target || prefetching_target)
        ? get_fd_entry(fd)
        : NULL;

    if (entry && entry->state.buffer_enabled && entry->wr_buf && entry->wr_pos > 0) {
        flush_on_reads++;
        if (flush_write_buffer(fd, entry->wr_buf, &entry->wr_pos, &entry->state) < 0) {
            return -1;
        }
    }

    if (!entry || !entry->state.prefetch_enabled) {
        ssize_t bytes = original_read(fd, buf, count);
        update_passthrough_reads(bytes > 0 ? (size_t)bytes : 0);
        if (entry) {
            advance_kernel_offset(&entry->state, bytes);
            advance_logical_offset(&entry->state, bytes);
        }
        return bytes;
    }

    if (!entry->pf_buf) {
        entry->pf_buf = malloc(prefetch_buffer_size);
        if (!entry->pf_buf) {
            ssize_t bytes = original_read(fd, buf, count);
            update_syscall_reads(bytes > 0 ? (size_t)bytes : 0);
            update_intercept_reads(bytes > 0 ? (size_t)bytes : 0);
            if (entry) {
                advance_kernel_offset(&entry->state, bytes);
                advance_logical_offset(&entry->state, bytes);
            }
            return bytes;
        }
        entry->pf_pos = 0;
        entry->pf_data_size = 0;
    }

    if (entry->pf_pos > entry->pf_data_size) {
        entry->pf_pos = entry->pf_data_size;
    }

    size_t available = entry->pf_data_size > entry->pf_pos
        ? (entry->pf_data_size - entry->pf_pos)
        : 0;

    if (count <= available) {
        memcpy(buf, entry->pf_buf + entry->pf_pos, count);
        entry->pf_pos += count;
        update_intercept_reads(count);
        advance_logical_offset(&entry->state, (ssize_t)count);
        return count;
    }

    char *out = buf;
    size_t total_read = 0;
    size_t needed = count;

    if (entry->pf_pos < entry->pf_data_size) {
        size_t remaining = entry->pf_data_size - entry->pf_pos;

        memcpy(out, entry->pf_buf + entry->pf_pos, remaining);

        entry->pf_pos += remaining;
        total_read += remaining;
        out += remaining;
        needed -= remaining;
    }

    if (needed > prefetch_buffer_size) {
        // The read bypasses the buffer, so its contents no longer surround the offset
        entry->pf_pos = 0;
        entry->pf_data_size = 0;

        ssize_t bytes = original_read(fd, out, needed);
        update_syscall_reads(bytes > 0 ? (size_t)bytes : 0);
        advance_kernel_offset(&entry->state, bytes);

        if (bytes <= 0) {
            update_intercept_reads(total_read);
            advance_logical_offset(&entry->state, (ssize_t)total_read);
            return total_read > 0 ? total_read : bytes;
        }

        total_read += bytes;
    } else {
        ssize_t bytes = original_read(fd, entry->pf_buf, prefetch_buffer_size);
        update_syscall_reads(bytes > 0 ? (size_t)bytes : 0);
        advance_kernel_offset(&entry->state, bytes);

        if (bytes <= 0) {
            update_intercept_reads(total_read);
            advance_logical_offset(&entry->state, (ssize_t)total_read);
            return total_read > 0 ? total_read : bytes;
        }

        entry->pf_data_size = (size_t)bytes;
        size_t to_copy = (size_t)bytes < needed ? (size_t)bytes : needed;
        memcpy(out, entry->pf_buf, to_copy);
        entry->pf_pos = to_copy;
        total_read += to_copy;
    }

    update_intercept_reads(total_read);
    advance_logical_offset(&entry->state, (ssize_t)total_read);

    return total_read;
}

/*
File management operations
*/

/*
Answer a seek from the tracked offsets when possible: tell() and seeks that
stay inside the prefetch window keep the buffers and skip the syscall.
Returns 1 and sets *result if handled.
*/

static int try_fast_seek(int fd, off_t offset, int whence, off_t *result) {
    fd_entry_t *entry = get_fd_entry(fd);
    if (!entry || !entry->state.seekable || entry->state.append) return 0;
    if (!entry->state.buffer_enabled && !entry->state.prefetch_enabled) return 0;

    off_t target;
    if (whence == SEEK_CUR) target = entry->state.logical_offset + offset;
    else if (whence == SEEK_SET) target = offset;
    else return 0;

    if (target == entry->state.logical_offset) {
        *result = target;
        return 1;
    }

    if (entry->pf_buf && entry->pf_data_size > 0) {
        off_t window_start = entry->state.logical_offset - (off_t)entry->pf_pos;
        off_t window_end = window_start + (off_t)entry->pf_data_size;
        if (target >= window_start && target <= window_end) {
            entry->pf_pos = (size_t)(target - window_start);
            entry->state.logical_offset = target;
            *result = target;
            return 1;
        }
    }

    return 0;
}

off_t lseek(int fd, off_t offset, int whence) {
    static original_lseek_t original_lseek = NULL;
    if (!original_lseek) original_lseek = (original_lseek_t)dlsym(RTLD_NEXT, "lseek");

    if (buffering_target || prefetching_target) {
        off_t fast;
        if (try_fast_seek(fd, offset, whence, &fast)) return fast;
        if (flush_buffers(fd) < 0) return -1;
    }

    off_t result = original_lseek(fd, offset, whence);

    if (result >= 0 && (buffering_target || prefetching_target)) {
        fd_entry_t *entry = get_or_create_fd_entry(fd);
        if (entry) {
            entry->state.kernel_offset = result;
            entry->state.logical_offset = result;
        }
    }

    return result;
}

off64_t lseek64(int fd, off64_t offset, int whence) {
    static original_lseek64_t original_lseek64 = NULL;
    if (!original_lseek64) original_lseek64 = (original_lseek64_t)dlsym(RTLD_NEXT, "lseek64");

    if (buffering_target || prefetching_target) {
        off_t fast;
        if (try_fast_seek(fd, (off_t)offset, whence, &fast)) return (off64_t)fast;
        if (flush_buffers(fd) < 0) return -1;
    }

    off64_t result = original_lseek64(fd, offset, whence);

    if (result >= 0 && (buffering_target || prefetching_target)) {
        fd_entry_t *entry = get_or_create_fd_entry(fd);
        if (entry) {
            entry->state.kernel_offset = (off_t)result;
            entry->state.logical_offset = (off_t)result;
        }
    }

    return result;
}

int fsync(int fd) {
    static original_fsync_t original_fsync = NULL;
    if (!original_fsync) original_fsync = dlsym(RTLD_NEXT, "fsync");

    if (buffering_target || prefetching_target) {
        if (flush_buffers(fd) < 0) return -1;
    }

    return original_fsync(fd);
}

int fdatasync(int fd) {
    static original_fdatasync_t original_fdatasync = NULL;
    if (!original_fdatasync) original_fdatasync = dlsym(RTLD_NEXT, "fdatasync");

    if (buffering_target || prefetching_target) {
        if (flush_buffers(fd) < 0) return -1;
    }

    return original_fdatasync(fd);
}

/*
dup2/dup3 silently close newfd, so flush and drop its entry like close()
does. oldfd is flushed so writes through the duplicate land after its
buffered data; the duplicate itself is not tracked.
*/
static void prepare_dup(int oldfd, int newfd) {
    if (!(buffering_target || prefetching_target)) return;

    (void)flush_buffers(oldfd);

    fd_entry_t *entry = get_fd_entry(newfd);
    if (entry) {
        if (entry->state.buffer_enabled && entry->wr_buf && entry->wr_pos > 0) {
            (void)flush_write_buffer(newfd, entry->wr_buf, &entry->wr_pos, &entry->state);
        }
        free_fd_entry(newfd);
    }
}

int dup2(int oldfd, int newfd) {
    static original_dup2_t original_dup2 = NULL;
    if (!original_dup2) original_dup2 = dlsym(RTLD_NEXT, "dup2");

    if (oldfd != newfd) prepare_dup(oldfd, newfd);

    return original_dup2(oldfd, newfd);
}

int dup3(int oldfd, int newfd, int flags) {
    static original_dup3_t original_dup3 = NULL;
    if (!original_dup3) original_dup3 = dlsym(RTLD_NEXT, "dup3");

    if (oldfd != newfd) prepare_dup(oldfd, newfd);

    return original_dup3(oldfd, newfd, flags);
}

pid_t fork(void) {
    static original_fork_t original_fork = NULL;
    if (!original_fork) original_fork = (original_fork_t)dlsym(RTLD_NEXT, "fork");

    pid_t pid = original_fork();

    if (pid == 0) {
        clear_all_fd_entries();
    }

    return pid;
}

/*
Clean up
*/

static void cleanup_buffers() {
    if (buffering_target) {
        for (size_t i = 0; i < fd_table_cap; i++) {
            fd_entry_t *entry = &fd_table[i];
            if (entry->state.fd >= 0 && entry->wr_buf) {
                (void)flush_write_buffer(entry->state.fd, entry->wr_buf, &entry->wr_pos, &entry->state);
            }
        }

        double intercept_wr_avg = intercept_writes
            ? (double)intercept_wr_bytes / (double)intercept_writes
            : 0.0;

        double syscall_wr_avg = syscall_writes
            ? (double)syscall_wr_bytes / (double)syscall_writes
            : 0.0;

        log_msg("write_stats intercepted calls=%lu bytes=%zu avg=%.2f\n", intercept_writes, intercept_wr_bytes, intercept_wr_avg);
        log_msg("write_stats executed calls=%lu bytes=%zu avg=%.2f\n", syscall_writes, syscall_wr_bytes, syscall_wr_avg);
        log_msg("write_stats passthrough calls=%lu bytes=%zu\n", passthrough_writes, passthrough_wr_bytes);
    }

    if (prefetching_target) {
        double intercept_rd_avg = intercept_reads
            ? (double)intercept_rd_bytes / (double)intercept_reads
            : 0.0;

        double syscall_rd_avg = syscall_reads
            ? (double)syscall_rd_bytes / (double)syscall_reads
            : 0.0;

        log_msg("read_stats intercepted calls=%lu bytes=%zu avg=%.2f\n", intercept_reads, intercept_rd_bytes, intercept_rd_avg);
        log_msg("read_stats executed calls=%lu bytes=%zu avg=%.2f\n", syscall_reads, syscall_rd_bytes, syscall_rd_avg);
        log_msg("read_stats passthrough calls=%lu bytes=%zu\n", passthrough_reads, passthrough_rd_bytes);
        log_msg("flush_on_reads=%lu\n", flush_on_reads);
    }

    for (size_t i = 0; i < fd_table_cap; i++) {
        fd_entry_t *entry = &fd_table[i];
        if (entry->state.fd >= 0) {
            free(entry->wr_buf);
            free(entry->pf_buf);
        }
    }

    free(fd_table);
    fd_table = NULL;
    fd_table_cap = 0;

    if (env_prefetch_targets) {
        free_target_list(env_prefetch_targets);
        env_prefetch_targets = NULL;
    }

    if (env_buffer_targets) {
        free_target_list(env_buffer_targets);
        env_buffer_targets = NULL;
    }

    if (env_file_targets) {
        free_target_list(env_file_targets);
        env_file_targets = NULL;
        file_target_list = NULL;
    }

    if (log_fd >= 0) {
        syscall(SYS_close, log_fd);
        log_fd = -1;
    }
}
