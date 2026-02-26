#define _GNU_SOURCE

#include <stdio.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

/*
Configuration
*/

#define WRITE_BUFFER_SIZE 8
#define MAX_WRITE_BUFFERS 5

#define PREFETCH_BUFFER_SIZE 16
#define MAX_PREFETCH_BUFFERS 5

/*
Function Pointer Types for Original System Calls
*/

typedef int (*original_open_t)(const char *path, int flags, ...);
typedef ssize_t (*original_write_t)(int fd, const void *buf, size_t count);
typedef ssize_t (*original_pwrite_t)(int fd, const void *buf, size_t count, off_t offset);
typedef ssize_t (*original_read_t)(int fd, void *buf, size_t count);
typedef int (*original_close_t)(int fd);
typedef off_t (*original_lseek_t)(int fd, off_t offset, int whence);
typedef int (*original_fsync_t)(int fd);
typedef int (*original_fdatasync_t)(int fd);

/*
Define Buffer Arrays
*/

typedef struct { // Write Buffers
    int fd;
    char *buffer;
    size_t buffer_pos;
} wr_buffer_t;

typedef struct { // Prefetch Buffers
    int fd;
    char *buffer;
    size_t buffer_pos;
    size_t data_size;
} pf_buffer_t;

static wr_buffer_t wr_buffers[MAX_WRITE_BUFFERS];
static pf_buffer_t pf_buffers[MAX_PREFETCH_BUFFERS];


__attribute__((constructor)) static void init_buffers() {
    for (int i = 0; i < MAX_WRITE_BUFFERS; i++) { // Initialize write buffers
        wr_buffers[i].fd = -1;
        wr_buffers[i].buffer = NULL;
        wr_buffers[i].buffer_pos = 0;
    }

    for (int i = 0; i < MAX_PREFETCH_BUFFERS; i++) { // Initialize prefetch buffers
        pf_buffers[i].fd = -1;
        pf_buffers[i].buffer = NULL;
        pf_buffers[i].buffer_pos = 0;
        pf_buffers[i].data_size = 0;
    }
}

/*
Utility Functions
*/

// Write Buffer Management

static wr_buffer_t* get_write_buffer(int fd) {
    if (fd < 2) { // Don't buffer std out or err
        return NULL; 
    }

    for (int i = 0; i < MAX_WRITE_BUFFERS; i++) {
        if (wr_buffers[i].fd == fd) {
            return &wr_buffers[i];
        }
    }
    return NULL;
}


static wr_buffer_t* allocate_write_buffer(int fd) {
    for (int i = 0; i < MAX_WRITE_BUFFERS; i++) {
        if (wr_buffers[i].fd == -1) {
            wr_buffers[i].buffer = malloc(WRITE_BUFFER_SIZE);
            if (!wr_buffers[i].buffer) {
                return NULL;
            }
            wr_buffers[i].fd = fd;
            wr_buffers[i].buffer_pos = 0;
            return &wr_buffers[i];
        }
    }
    return NULL;
}


static void flush_write_buffer(wr_buffer_t *wb) {
    static original_write_t original_write = NULL;
    if (!original_write) original_write = dlsym(RTLD_NEXT, "write");

    if (wb->buffer_pos > 0 && wb->fd >= 0) {
        original_write(wb->fd, wb->buffer, wb->buffer_pos); // Possibly add error handling
        wb->buffer_pos = 0;
    }
}


static void free_write_buffer(wr_buffer_t * wb) {
    flush_write_buffer(wb);
    free(wb->buffer);
    wb->buffer = NULL;
    wb->fd = -1;
    wb->buffer_pos = 0;
    return;
}

// Prefetch Buffer Management

static pf_buffer_t* get_prefetch_buffer(int fd) {
    for (int i = 0; i < MAX_PREFETCH_BUFFERS; i++) {
        if (pf_buffers[i].fd == fd) {
            return &pf_buffers[i];
        }
    }
    return NULL;
}


static pf_buffer_t* allocate_prefetch_buffer(int fd) {
    for (int i = 0; i < MAX_PREFETCH_BUFFERS; i++) {
        if (pf_buffers[i].fd == -1) {
            pf_buffers[i].buffer = malloc(PREFETCH_BUFFER_SIZE);
            if (!pf_buffers[i].buffer) {
                return NULL;
            }
            pf_buffers[i].fd = fd;
            pf_buffers[i].buffer_pos = 0;
            pf_buffers[i].data_size = 0;
            return &pf_buffers[i];
        }
    }
    return NULL;
}


static void free_prefetch_buffer(pf_buffer_t * pb) {
    free(pb->buffer);
    pb->buffer = NULL;
    pb->fd = -1;
    pb->buffer_pos = 0;
    pb->data_size = 0;
    return;
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

    return fd;
}


int close(int fd) {
    static original_close_t original_close = NULL;
    if (!original_close) original_close = dlsym(RTLD_NEXT, "close");

    wr_buffer_t *wb = get_write_buffer(fd);
    if (wb) {
        free_write_buffer(wb);
    }

    pf_buffer_t *pb = get_prefetch_buffer(fd);
    if (pb) {
        free_prefetch_buffer(pb);
    }

    int result = original_close(fd);

    return result;
}

// Write operations

ssize_t write(int fd, const void *buf, size_t count) {
    static original_write_t original_write = NULL;
    if (!original_write) original_write = dlsym(RTLD_NEXT, "write");

    if (fd <= 2) { //checking this twice...
        return original_write(fd, buf, count);
    }

    wr_buffer_t *wb = get_write_buffer(fd);

    if (!wb) {
        wb = allocate_write_buffer(fd);
        if (!wb) {
            return original_write(fd, buf, count);
        }
    }

    if (wb->buffer_pos + count > WRITE_BUFFER_SIZE) {
        flush_write_buffer(wb);

        if (count > WRITE_BUFFER_SIZE) {
            return original_write(fd, buf, count);
        }
    }
    
    memcpy(wb->buffer + wb->buffer_pos, buf, count);
    wb->buffer_pos += count;
    
    return count;
}

ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset) {
    static original_pwrite_t original_pwrite = NULL;
    if (!original_pwrite) original_pwrite = dlsym(RTLD_NEXT, "pwrite");
    
    wr_buffer_t *wb = get_write_buffer(fd);
    if (wb && wb->buffer_pos > 0) {
        flush_write_buffer(wb);
    }
    
    pf_buffer_t *pb = get_prefetch_buffer(fd);
    if (pb) {
        pb->buffer_pos = 0;
        pb->data_size = 0;
    }

    return original_pwrite(fd, buf, count, offset);
}

// Read operations


ssize_t read(int fd, void *buf, size_t count) {
    static original_read_t original_read = NULL;
    if (!original_read) original_read = dlsym(RTLD_NEXT, "read");

    pf_buffer_t *pb = get_prefetch_buffer(fd);

    if (!pb) {
        pb = allocate_prefetch_buffer(fd);
        if (!pb) {
            return original_read(fd, buf, count);
        }
    }

    if (count <= pb->data_size - pb->buffer_pos) { // If requested data is in buffer
        memcpy(buf, pb->buffer + pb->buffer_pos, count);
        pb->buffer_pos += count;
        return count;
    }

    char *out = buf;
    size_t total_read = 0;
    size_t needed = count;

    if (pb->buffer_pos < pb->data_size) { // If some data is in buffer
        size_t remaining = pb->data_size - pb->buffer_pos;
        
        memcpy(out, pb->buffer + pb->buffer_pos, remaining);

        pb->buffer_pos += remaining;
        total_read += remaining;
        out += remaining;
        needed -= remaining;
    }

    if (needed == 0) { // If no more data is needed
        return total_read;
    }

    if (needed > PREFETCH_BUFFER_SIZE) {
        ssize_t bytes = original_read(fd, out, needed);

        if (bytes <= 0) {
            return total_read > 0 ? total_read : bytes; // Return what we have or the error
        }

        total_read += bytes;
    } else {
        ssize_t bytes = original_read(fd, pb->buffer, PREFETCH_BUFFER_SIZE);
    
        if (bytes <= 0) {
            return total_read > 0 ? total_read : bytes;
        } else if (bytes < needed) {
            needed = bytes; // Adjust needed if we read less than requested
        }
    
        pb->data_size = bytes;
        memcpy(out, pb->buffer, needed);
        pb->buffer_pos = needed;
        total_read += needed;
    }

    return total_read;
}

/*
File management operations
*/

off_t lseek(int fd, off_t offset, int whence) {
    static original_lseek_t original_lseek = NULL;
    if (!original_lseek) original_lseek = (original_lseek_t)dlsym(RTLD_NEXT, "lseek");

    wr_buffer_t *wb = get_write_buffer(fd);
    if (wb && wb->buffer_pos > 0) {
        flush_write_buffer(wb);
    }

    pf_buffer_t * pb = get_prefetch_buffer(fd);
    if (pb) {
        pb->buffer_pos = 0;
        pb->data_size = 0;
    }

    return original_lseek(fd, offset, whence);
}


int fsync(int fd) {
    static original_fsync_t original_fsync = NULL;
    if (!original_fsync) original_fsync = dlsym(RTLD_NEXT, "fsync");

    wr_buffer_t *wb = get_write_buffer(fd);
    if (wb && wb->buffer_pos > 0) {
        flush_write_buffer(wb)
    }

    pf_buffer_t * pb = get_prefetch_buffer(fd);
    if (pb) {
        pb->buffer_pos = 0;
        pb->data_size = 0;
    }

    return original_fsync(fd);
}


int fdatasync(int fd) {
    static original_fdatasync_t original_fdatasync = NULL;
    if (!original_fdatasync) original_fdatasync = dlsym(RTLD_NEXT, "fdatasync");

    wr_buffer_t *wb = get_write_buffer(fd);
    if (wb && wb->buffer_pos > 0) {
        flush_write_buffer(wb);
    }

    pf_buffer_t * pb = get_prefetch_buffer(fd);
    if (pb) {
        pb->buffer_pos = 0;
        pb->data_size = 0;
    }

    return original_fdatasync(fd);
}


/*
Clean up
*/

__attribute__((destructor)) static void cleanup_buffers() {
    for (int i = 0; i < MAX_WRITE_BUFFERS; i++) {
        if (wr_buffers[i].buffer) {
            flush_write_buffer(&wr_buffers[i]);
            free(wr_buffers[i].buffer);
            wr_buffers[i].buffer = NULL;
            wr_buffers[i].fd = -1;
            wr_buffers[i].buffer_pos = 0;
        }
    }
    for (int i = 0; i < MAX_PREFETCH_BUFFERS; i++) {
        if (pf_buffers[i].buffer) {
            free(pf_buffers[i].buffer);
            pf_buffers[i].buffer = NULL;
            pf_buffers[i].fd = -1;
            pf_buffers[i].buffer_pos = 0;
            pf_buffers[i].data_size = 0;
        }
    }
}