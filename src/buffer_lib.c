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

/*
Configuration
*/

#define ENABLE_WRITE_BUFFERING 1
#define ENABLE_READ_PREFETCH 1

#define WRITE_BUFFER_SIZE (64 * 1024) // 64KB
#define INITIAL_WRITE_BUFFER_COUNT 64

#define PREFETCH_BUFFER_SIZE (64 * 1024) // 64KB
#define INITIAL_PREFETCH_BUFFER_COUNT 64

#define INITIAL_FD_STATE_COUNT 64

#define LOG_FILE "/mnt/buffer.log"

static char *prefetching_targets[] = {
    "dwidenoise",
    "fslmaths",
    NULL
};

static char *buffering_targets[] = {
    "dwidenoise",
    "fslmaths",
    NULL
};

static char *log_file_path = LOG_FILE;
static char **prefetch_target_list = prefetching_targets;
static char **buffer_target_list = buffering_targets;
static char **env_prefetch_targets = NULL;
static char **env_buffer_targets = NULL;

/*
Function Pointer Types for Original System Calls
*/

typedef int (*original_open_t)(const char *path, int flags, ...);
typedef ssize_t (*original_write_t)(int fd, const void *buf, size_t count);
typedef ssize_t (*original_pwrite_t)(int fd, const void *buf, size_t count, off_t offset);
typedef ssize_t (*original_read_t)(int fd, void *buf, size_t count);
typedef int (*original_close_t)(int fd);
typedef off_t (*original_lseek_t)(int fd, off_t offset, int whence);
typedef off64_t (*original_lseek64_t)(int fd, off64_t offset, int whence);
typedef int (*original_fsync_t)(int fd);
typedef int (*original_fdatasync_t)(int fd);
typedef pid_t (*original_fork_t)(void);

/*
Define Buffer Arrays
*/

typedef struct {
    int fd;
    off_t logical_offset;
    off_t kernel_offset;
    int seekable;
} fd_state_t;

typedef struct { // Write Buffers
    int fd;
    char *buffer;
    size_t buffer_pos;
    fd_state_t *state;
} wr_buffer_t;

typedef struct { // Prefetch Buffers
    int fd;
    char *buffer;
    size_t buffer_pos;
    size_t data_size;
    fd_state_t *state;
} pf_buffer_t;

static wr_buffer_t **wr_buffers = NULL;
static size_t wr_buffer_cap = 0;

static pf_buffer_t **pf_buffers = NULL;
static size_t pf_buffer_cap = 0;

static fd_state_t **fd_states = NULL;
static size_t fd_state_cap = 0;

/*
Statistics Variables
*/

// Write Stats
static unsigned long intercept_writes = 0;
static unsigned long syscall_writes = 0;

static size_t intercept_wr_bytes = 0;
static size_t syscall_wr_bytes = 0;

//Read Stats
static unsigned long intercept_reads = 0;
static unsigned long syscall_reads = 0;

static size_t intercept_rd_bytes = 0;
static size_t syscall_rd_bytes = 0;

static unsigned long flush_on_reads = 0;

/*
Forward Declarations
*/

static void log_msg(const char *fmt, ...);
static int get_cmdline(char *out, size_t out_size);
static int is_buffering_target();
static int is_prefetching_target();
static void init_config_from_env(void);
static char **split_target_list(const char *value);
static void free_target_list(char **list);

static char cmdline[512];
static int buffering_target = 0;
static int prefetching_target = 0;

__attribute__((constructor)) static void init_buffers();
__attribute__((destructor)) static void cleanup_buffers();

/*
Intialization
*/

static void init_buffers() {
    get_cmdline(cmdline, sizeof(cmdline));

    init_config_from_env();
    
    if (ENABLE_WRITE_BUFFERING && is_buffering_target()) buffering_target = 1;
    if (ENABLE_READ_PREFETCH && is_prefetching_target()) prefetching_target = 1;

    if (!buffering_target && !prefetching_target) return;

    log_msg("Initializing buffer library...\n");
    log_msg("%s\n", cmdline);
    log_msg("Buffering: %d\n", buffering_target);
    log_msg("Prefetching: %d\n", prefetching_target);

    if (buffering_target) {
        wr_buffer_cap = INITIAL_WRITE_BUFFER_COUNT;
        wr_buffers = calloc(wr_buffer_cap, sizeof(wr_buffer_t*));
        if (!wr_buffers) {
            log_msg("Failed to allocate write buffer array\n");
            wr_buffer_cap = 0;
        }
    } else {
        wr_buffer_cap = 0;
        wr_buffers = NULL;
    }

    if (prefetching_target) {
        pf_buffer_cap = INITIAL_PREFETCH_BUFFER_COUNT;
        pf_buffers = calloc(pf_buffer_cap, sizeof(pf_buffer_t*));
        if (!pf_buffers) {
            log_msg("Failed to allocate prefetch buffer array\n");
            pf_buffer_cap = 0;
        }
    } else {
        pf_buffer_cap = 0;
        pf_buffers = NULL;
    }
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

static int is_target_delim(char c) {
    return c == ',' || c == ' ' || c == '\t' || c == '\n' || c == ';';
}

static char **split_target_list(const char *value) {
    if (!value) return NULL;

    while (*value && is_target_delim(*value)) {
        value++;
    }
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
}


static int is_prefetching_target() {
    if (!prefetch_target_list) return 0;

    for (char **t = prefetch_target_list; *t; t++) {
        if (strstr(cmdline, *t)) {
            return 1;
        }
    }
    return 0;
}


static int is_buffering_target() {
    if (!buffer_target_list) return 0;

    for (char **t = buffer_target_list; *t; t++) {
        if (strstr(cmdline, *t)) {
            return 1;
        }
    }
    return 0;
}

// Statistics Functions

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

// Offset Tracking

static int ensure_fd_state_cap(int fd) {
    if (fd < 0) return 0;

    if ((size_t)fd < fd_state_cap) return 1;

    size_t new_cap = fd_state_cap ? fd_state_cap : INITIAL_FD_STATE_COUNT;
    while (new_cap <= (size_t)fd) new_cap *= 2;

    fd_state_t **new_array = realloc(fd_states, new_cap * sizeof(*new_array));
    if (!new_array) return 0;

    memset(new_array + fd_state_cap, 0, (new_cap - fd_state_cap) * sizeof(*new_array));
    fd_states = new_array;
    fd_state_cap = new_cap;
    return 1;
}

static fd_state_t *get_fd_state(int fd) {
    if (fd < 0) return NULL;
    if ((size_t)fd >= fd_state_cap) return NULL;
    return fd_states[fd];
}

static fd_state_t *allocate_fd_state(int fd) {
    if (fd < 0) return NULL;
    if (!ensure_fd_state_cap(fd)) return NULL;

    fd_state_t *state = fd_states[fd];
    if (state) return state;

    state = calloc(1, sizeof(*state));
    if (!state) return NULL;

    state->fd = fd;
    state->logical_offset = 0;
    state->kernel_offset = 0;
    state->seekable = 1;

    int saved_errno = errno;
    off_t cur = syscall(SYS_lseek, fd, 0, SEEK_CUR);
    if (cur >= 0) {
        state->logical_offset = cur;
        state->kernel_offset = cur;
        state->seekable = 1;
    } else if (errno == ESPIPE) {
        state->seekable = 0;
    }
    errno = saved_errno;

    fd_states[fd] = state;
    return state;
}

static fd_state_t *get_or_create_fd_state(int fd) {
    if (fd < 2) return NULL;
    fd_state_t *state = get_fd_state(fd);
    if (state) return state;
    return allocate_fd_state(fd);
}

static void free_fd_state(int fd) {
    if (fd < 0) return;
    if ((size_t)fd >= fd_state_cap) return;
    if (!fd_states[fd]) return;
    free(fd_states[fd]);
    fd_states[fd] = NULL;
}

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

// Write Buffer Management

static int ensure_wr_cap(int fd) {
    if (fd < 0) return 0;

    if (fd < wr_buffer_cap) return 1; //fd is contained in array

    size_t new_cap = wr_buffer_cap ? wr_buffer_cap : INITIAL_WRITE_BUFFER_COUNT; //Use pre-existing cap or use initial count
    while (new_cap <= fd) new_cap *= 2;

    wr_buffer_t **new_array = realloc(wr_buffers, new_cap * sizeof(*new_array)); //Reallocate the wr_buffers array with the size of new_cap
    if (!new_array) return 0;

    memset(new_array + wr_buffer_cap, 0, (new_cap - wr_buffer_cap) * sizeof(*new_array)); //Initialize new part of array as 0
    wr_buffers = new_array;
    wr_buffer_cap = new_cap;
    return 1;
}


static wr_buffer_t* get_write_buffer(int fd) {
    if (fd < 2) return NULL; // Don't buffer std out or err

    if((size_t)fd >= wr_buffer_cap) return NULL;

    return wr_buffers[fd];
}


static wr_buffer_t* allocate_write_buffer(int fd) {
    if (fd < 2) return NULL;
    if (!ensure_wr_cap(fd)) return NULL;

    wr_buffer_t *wb = calloc(1, sizeof(wr_buffer_t));
    if (!wb) return NULL;    
    
    wb->fd = fd;
    wb->buffer = malloc(WRITE_BUFFER_SIZE);
    if (!wb->buffer) {
        free(wb);
        return NULL;
    }
    wb->buffer_pos = 0;
    wb->state = get_or_create_fd_state(fd);

    wr_buffers[fd] = wb;
    return wb;
}


static int flush_write_buffer(wr_buffer_t *wb) {
    static original_write_t original_write = NULL;
    if (!original_write) original_write = dlsym(RTLD_NEXT, "write");

    if (wb->buffer_pos == 0 || wb->fd < 0) return 0;

    fd_state_t *state = wb->state;

    size_t remaining = wb->buffer_pos;
    size_t offset = 0;

    while (remaining > 0) {
        ssize_t bytes = original_write(wb->fd, wb->buffer + offset, remaining);
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

    wb->buffer_pos = 0;
    return 0;
}


static void free_write_buffer(wr_buffer_t *wb) { 
    wr_buffers[wb->fd] = NULL;
    free(wb->buffer);
    free(wb);
}

// Prefetch Buffer Management

static int ensure_pf_cap(int fd) {
    if (fd < 0) return 0;

    if (fd < pf_buffer_cap) return 1; //fd is contained in array

    size_t new_cap = pf_buffer_cap ? pf_buffer_cap : INITIAL_PREFETCH_BUFFER_COUNT;
    while (new_cap <= fd) new_cap *= 2;

    pf_buffer_t **new_array = realloc(pf_buffers, new_cap * sizeof(*new_array));
    if (!new_array) return 0;

    memset(new_array + pf_buffer_cap, 0, (new_cap - pf_buffer_cap) * sizeof(*new_array));
    pf_buffers = new_array;
    pf_buffer_cap = new_cap;
    return 1;
}


static pf_buffer_t* get_prefetch_buffer(int fd) {
    if (fd < 2) return NULL; // Don't buffer std in
    if ((size_t)fd >= pf_buffer_cap) return NULL;

    return pf_buffers[fd];
}


static pf_buffer_t* allocate_prefetch_buffer(int fd) {
    if (fd < 2) return NULL;
    if (!ensure_pf_cap(fd)) return NULL;

    pf_buffer_t *pb = calloc(1, sizeof(pf_buffer_t));
    if (!pb) return NULL;
    
    pb->fd = fd;
    pb->buffer = malloc(PREFETCH_BUFFER_SIZE);
    if (!pb->buffer) {
        free(pb);
        return NULL;
    }
    pb->buffer_pos = 0;
    pb->state = get_or_create_fd_state(fd);

    pf_buffers[fd] = pb;
    return pb;
}


static void free_prefetch_buffer(pf_buffer_t *pb) {
    pf_buffers[pb->fd] = NULL;
    free(pb->buffer);
    free(pb);
}

// General Buffer Management

static int flush_buffers(int fd) {
    wr_buffer_t *wb = get_write_buffer(fd);
    if (wb && wb->buffer_pos > 0 && flush_write_buffer(wb) < 0) {
        return -1;
    }

    pf_buffer_t *pb = get_prefetch_buffer(fd);
    if (pb) {
        if (sync_kernel_to_logical(fd, pb->state) < 0) {
            return -1;
        }
        pb->buffer_pos = 0;
        pb->data_size = 0;
    }

    return 0;
}


static void clear_write_buffers(void) { //Clear all write buffers for child after fork
    for (int i = 0; i < wr_buffer_cap; i++) {
        wr_buffer_t *wb = get_write_buffer(i);
        if (wb) {
            free_write_buffer(wb);
        }
    }
}

static void clear_prefetch_buffers(void) { // Clear all prefetch buffers for child after fork
    for (int i = 0; i < pf_buffer_cap; i++) {
        pf_buffer_t *pb = get_prefetch_buffer(i);
        if (pb) {
            free_prefetch_buffer(pb);
        }
    }
}

static void clear_fd_states(void) {
    for (int i = 0; i < fd_state_cap; i++) {
        if (fd_states[i]) {
            free(fd_states[i]);
            fd_states[i] = NULL;
        }
    }
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
        (void)allocate_fd_state(fd);
    }

    return fd;
}


int close(int fd) {
    static original_close_t original_close = NULL;
    if (!original_close) original_close = dlsym(RTLD_NEXT, "close");

    int flush_errno = 0;

    wr_buffer_t *wb = get_write_buffer(fd);
    if (wb) {
        if (flush_write_buffer(wb) < 0) {
            flush_errno = errno;
        }
        free_write_buffer(wb);
    }

    pf_buffer_t *pb = get_prefetch_buffer(fd);
    if (pb) {
        free_prefetch_buffer(pb);
    }

    free_fd_state(fd);

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

    if (fd < 2) { //checking this twice...
        return original_write(fd, buf, count);
    }

    fd_state_t *state = (buffering_target || prefetching_target)
        ? get_or_create_fd_state(fd)
        : NULL;

    if (!buffering_target) {
        ssize_t bytes = original_write(fd, buf, count);
        update_syscall_writes(bytes > 0 ? (size_t)bytes : 0);
        advance_kernel_offset(state, bytes);
        advance_logical_offset(state, bytes);
        return bytes;
    }

    update_intercept_writes(count);

    wr_buffer_t *wb = get_write_buffer(fd);

    if (!wb) {
        wb = allocate_write_buffer(fd);
        if (!wb) {
            ssize_t bytes = original_write(fd, buf, count);
            update_syscall_writes(bytes > 0 ? (size_t)bytes : 0);
            advance_kernel_offset(state, bytes);
            advance_logical_offset(state, bytes);
            return bytes;
        }
    }

    if (wb->buffer_pos + count > WRITE_BUFFER_SIZE) {
        if (flush_write_buffer(wb) < 0) {
            return -1;
        }

        if (count > WRITE_BUFFER_SIZE) {
            ssize_t bytes = original_write(fd, buf, count);
            update_syscall_writes(bytes > 0 ? (size_t)bytes : 0);
            advance_kernel_offset(state, bytes);
            advance_logical_offset(state, bytes);
            return bytes;
        }
    }
    
    memcpy(wb->buffer + wb->buffer_pos, buf, count);
    wb->buffer_pos += count;
    advance_logical_offset(state, (ssize_t)count);
    
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

    fd_state_t *state = (buffering_target || prefetching_target)
        ? get_or_create_fd_state(fd)
        : NULL;

    // Flush write buffer before reading to maintain read-after-write consistency
    if (buffering_target) {
        wr_buffer_t *wb = get_write_buffer(fd);
        
        if (wb && wb->buffer_pos > 0) {
            flush_on_reads++;
            if (flush_write_buffer(wb) < 0) return -1;
        }
    }

    if (!prefetching_target) {
        ssize_t bytes = original_read(fd, buf, count);
        update_syscall_reads(bytes > 0 ? (size_t)bytes : 0);
        advance_kernel_offset(state, bytes);
        advance_logical_offset(state, bytes);
        return bytes;
    }

    pf_buffer_t *pb = get_prefetch_buffer(fd);

    if (!pb) {
        pb = allocate_prefetch_buffer(fd);
        if (!pb) {
            ssize_t bytes = original_read(fd, buf, count);
            update_syscall_reads(bytes > 0 ? (size_t)bytes : 0);
            update_intercept_reads(bytes > 0 ? (size_t)bytes : 0);
            advance_kernel_offset(state, bytes);
            advance_logical_offset(state, bytes);
            return bytes;
        }
    }

    if (pb->buffer_pos > pb->data_size) {
        pb->buffer_pos = pb->data_size;
    }

    if (pb->state) {
        state = pb->state;
    }

    size_t available = pb->data_size > pb->buffer_pos
        ? (pb->data_size - pb->buffer_pos)
        : 0;
    
    // If requested data is in buffer
    if (count <= available) {
        memcpy(buf, pb->buffer + pb->buffer_pos, count);
        pb->buffer_pos += count;
        update_intercept_reads(count);
        advance_logical_offset(state, (ssize_t)count);
        return count;
    }

    char *out = buf;
    size_t total_read = 0;
    size_t needed = count;

    // If some data is in buffer
    if (pb->buffer_pos < pb->data_size) {
        size_t remaining = pb->data_size - pb->buffer_pos;
        
        memcpy(out, pb->buffer + pb->buffer_pos, remaining);

        pb->buffer_pos += remaining;
        total_read += remaining;
        out += remaining;
        needed -= remaining;
    }

    if (needed > PREFETCH_BUFFER_SIZE) {
        ssize_t bytes = original_read(fd, out, needed);
        update_syscall_reads(bytes > 0 ? (size_t)bytes : 0);
        advance_kernel_offset(state, bytes);

        if (bytes <= 0) {
            update_intercept_reads(total_read);
            advance_logical_offset(state, (ssize_t)total_read);
            return total_read > 0 ? total_read : bytes; // Return what we have or the error
        }

        total_read += bytes;
    } else {
        ssize_t bytes = original_read(fd, pb->buffer, PREFETCH_BUFFER_SIZE);
        update_syscall_reads(bytes > 0 ? (size_t)bytes : 0);
        advance_kernel_offset(state, bytes);
    
        if (bytes <= 0) {
            update_intercept_reads(total_read);
            advance_logical_offset(state, (ssize_t)total_read);
            return total_read > 0 ? total_read : bytes;
        }
    
        pb->data_size = (size_t)bytes;
        size_t to_copy = (size_t)bytes < needed ? (size_t)bytes : needed;
        memcpy(out, pb->buffer, to_copy);
        pb->buffer_pos = to_copy;
        total_read += to_copy;
    }

    update_intercept_reads(total_read);
    advance_logical_offset(state, (ssize_t)total_read);

    return total_read;
}

/*
File management operations
*/

off_t lseek(int fd, off_t offset, int whence) {
    static original_lseek_t original_lseek = NULL;
    if (!original_lseek) original_lseek = (original_lseek_t)dlsym(RTLD_NEXT, "lseek");

    fd_state_t *state = (buffering_target || prefetching_target)
        ? get_or_create_fd_state(fd)
        : NULL;

    if (buffering_target || prefetching_target) {
        if (flush_buffers(fd) < 0) return -1;
    }

    off_t result = original_lseek(fd, offset, whence);
    if (state && result >= 0) {
        state->kernel_offset = result;
        state->logical_offset = result;
    }
    return result;
}


off64_t lseek64(int fd, off64_t offset, int whence) {
    static original_lseek64_t original_lseek64 = NULL;
    if (!original_lseek64) original_lseek64 = (original_lseek64_t)dlsym(RTLD_NEXT, "lseek64");

    fd_state_t *state = (buffering_target || prefetching_target)
        ? get_or_create_fd_state(fd)
        : NULL;

    if (buffering_target || prefetching_target) {
        if (flush_buffers(fd) < 0) return -1;
    }

    off64_t result = original_lseek64(fd, offset, whence);
    if (state && result >= 0) {
        state->kernel_offset = (off_t)result;
        state->logical_offset = (off_t)result;
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


pid_t fork(void) {
    static original_fork_t original_fork = NULL;
    if (!original_fork) original_fork = (original_fork_t)dlsym(RTLD_NEXT, "fork");

    pid_t pid = original_fork();

    if (pid == 0) {
        if (buffering_target) clear_write_buffers();
        if (prefetching_target) clear_prefetch_buffers();
        clear_fd_states();
    }

    return pid;
}

/*
Clean up
*/

static void cleanup_buffers() {
    if (buffering_target) {
        for (int i = 0; i < wr_buffer_cap; i++) {
            if (wr_buffers[i]) {
                (void)flush_write_buffer(wr_buffers[i]);
                free_write_buffer(wr_buffers[i]);
            }
        }
        
        free(wr_buffers);
        wr_buffers = NULL;
        wr_buffer_cap = 0;
        
        double intercept_wr_avg = intercept_writes
            ? (double)intercept_wr_bytes / (double)intercept_writes
            : 0.0;

        double syscall_wr_avg = syscall_writes
            ? (double)syscall_wr_bytes / (double)syscall_writes
            : 0.0;

        log_msg("write_stats intercepted calls=%lu bytes=%zu avg=%.2f\n", intercept_writes, intercept_wr_bytes, intercept_wr_avg);
        log_msg("write_stats executed calls=%lu bytes=%zu avg=%.2f\n", syscall_writes, syscall_wr_bytes, syscall_wr_avg);
    }

    if (prefetching_target) {
        for (int i = 0; i < pf_buffer_cap; i++) {
            if (pf_buffers[i]) {
                free_prefetch_buffer(pf_buffers[i]);
            }
        }

        free(pf_buffers);
        pf_buffers = NULL;
        pf_buffer_cap = 0;

        double intercept_rd_avg = intercept_reads
            ? (double)intercept_rd_bytes / (double)intercept_reads
            : 0.0;

        double syscall_rd_avg = syscall_reads
            ? (double)syscall_rd_bytes / (double)syscall_reads
            : 0.0;

        log_msg("read_stats intercepted calls=%lu bytes=%zu avg=%.2f\n", intercept_reads, intercept_rd_bytes, intercept_rd_avg);
        log_msg("read_stats executed calls=%lu bytes=%zu avg=%.2f\n", syscall_reads, syscall_rd_bytes, syscall_rd_avg);
        log_msg("flush_on_reads=%lu\n", flush_on_reads);
    }

    if (env_prefetch_targets) {
        free_target_list(env_prefetch_targets);
        env_prefetch_targets = NULL;
    }

    if (env_buffer_targets) {
        free_target_list(env_buffer_targets);
        env_buffer_targets = NULL;
    }

    if (fd_states) {
        for (int i = 0; i < fd_state_cap; i++) {
            if (fd_states[i]) {
                free(fd_states[i]);
            }
        }
        free(fd_states);
        fd_states = NULL;
        fd_state_cap = 0;
    }
    
    if (log_fd >= 0) {
        syscall(SYS_close, log_fd);
        log_fd = -1;
    }
}