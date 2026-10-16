#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
#include <fnmatch.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>
#include <zlib.h>
#include <zstd.h>

// zlib.h makes gzgetc a macro that reads the handle's fields directly; the
// library defines the real function instead
#undef gzgetc

// Slots in the handle table at first; it doubles when full
#define INITIAL_HANDLES 16
#define DEFAULT_LEVEL 1
// Size of each handle's buffers, so also of its write() and read() calls
#define DEFAULT_BUFFER_SIZE (128 * 1024)
// Decompressed bytes kept from the start of each file read (compression_lib's
// READ_LAZY_LIMIT), so a rewind to read the header again is free
#define DEFAULT_HEADER_CACHE (1024 * 1024)
// Contexts kept after gzclose for the next file
#define POOL_SIZE 4
// Characters gzungetc can push back (zlib guarantees one)
#define UNGET_SIZE 64
// ZSTD_WINDOWLOG_LIMIT_DEFAULT, which 1.4.8 only declares for static linking:
// the largest window a decoder accepts unless told otherwise
#define WINDOW_LOG_LIMIT_DEFAULT 27


typedef gzFile (*original_gzopen_t)(const char *path, const char *mode);
typedef gzFile (*original_gzdopen_t)(int fd, const char *mode);
typedef int (*original_gzbuffer_t)(gzFile file, unsigned size);
typedef int (*original_gzsetparams_t)(gzFile file, int level, int strategy);
typedef int (*original_gzwrite_t)(gzFile file, const void *buf, unsigned int len);
typedef z_size_t (*original_gzfwrite_t)(voidpc buf, z_size_t size, z_size_t nitems, gzFile file);
typedef int (*original_gzvprintf_t)(gzFile file, const char *format, va_list va);
typedef int (*original_gzputs_t)(gzFile file, const char *s);
typedef int (*original_gzputc_t)(gzFile file, int c);
typedef int (*original_gzflush_t)(gzFile file, int flush);
typedef int (*original_gzread_t)(gzFile file, voidp buf, unsigned len);
typedef z_size_t (*original_gzfread_t)(voidp buf, z_size_t size, z_size_t nitems, gzFile file);
typedef int (*original_gzgetc_t)(gzFile file);
typedef int (*original_gzungetc_t)(int c, gzFile file);
typedef char *(*original_gzgets_t)(gzFile file, char *buf, int len);
typedef z_off_t (*original_gzseek_t)(gzFile file, z_off_t offset, int whence);
typedef z_off64_t (*original_gzseek64_t)(gzFile file, z_off64_t offset, int whence);
typedef int (*original_gzrewind_t)(gzFile file);
typedef z_off_t (*original_gztell_t)(gzFile file);
typedef z_off64_t (*original_gztell64_t)(gzFile file);
typedef int (*original_gzeof_t)(gzFile file);
typedef int (*original_gzdirect_t)(gzFile file);
typedef const char *(*original_gzerror_t)(gzFile file, int *errnum);
typedef void (*original_gzclearerr_t)(gzFile file);
typedef int (*original_gzclose_t)(gzFile file);


enum { WRITE_STREAM = 1, READ_STREAM };

// A gz file taken over from zlib and streamed through zstd: writes to target
// files, and reads of any file that starts with the zstd magic number.
// x comes first and stays zero: zlib's gzgetc macro reads it as its count of
// buffered bytes, and 0 sends every call to the gzgetc function
typedef struct{
    struct gzFile_s x;
    int mode;
    int fd;
    char *path;             // for messages; "<fd:N>" after gzdopen
    int err;                // Z_OK, or the error gzerror reports
    char *msg;
    z_off64_t upos;         // uncompressed position, what gztell returns
    unsigned buf_size;      // size of in and out, set by gzbuffer before the first I/O
    unsigned char *in;      // read: compressed input
    unsigned char *out;     // write: compressed output; read: decompressed output

    // Write
    ZSTD_CCtx *cctx;
    int level;
    size_t out_pos;         // compressed bytes waiting in out

    // Read
    ZSTD_DCtx *dctx;
    off_t start;            // file offset of the first frame (not 0 after gzdopen)
    size_t in_pos, in_end;  // unread part of in
    int raw_eof;            // read() returned 0
    int frame_open;         // the decoder is inside a frame
    int end;                // every frame is decoded
    int past;               // a read asked for more than was left (gzeof)
    z_off64_t dpos;         // uncompressed position the decoder has reached
    size_t out_len;         // bytes decoded into out, ending at dpos
    unsigned char *cache;   // the first cache_limit bytes of the file
    size_t cache_len, cache_limit;
    unsigned char back[UNGET_SIZE]; // gzungetc's characters, read before upos
    int nback;
    int rewinds;            // times the decoder went back to the first frame
    int forked;             // a copy of the parent's handle in a fork() child

    // For the line printed at gzclose
    long long raw_bytes;    // compressed bytes written or read
    long long data_bytes;   // uncompressed bytes compressed or decoded
    double secs;            // time spent compressing or decoding, syscalls included
} zstd_handle_t;

// lock guards the handle table and the context pools. Each handle belongs to
// one thread at a time, as zlib's do, so its own fields need no lock
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static zstd_handle_t **handles;
static int handle_slots;
static int handle_count;    // also read without the lock, atomically, to skip empty tables

static ZSTD_CCtx *cctx_pool[POOL_SIZE];
static ZSTD_DCtx *dctx_pool[POOL_SIZE];
static int cctx_pooled, dctx_pooled;

// Per-call prints only with ZSTD_DEBUG set (read once, in init)
static int debug;


/* Helpers */

static int debug_enabled(void){
    return debug;
}

// ZSTD_LEVEL sets the level; the zlib level in the mode string is ignored
static int zstd_level(void){
    const char *env = getenv("ZSTD_LEVEL");
    if (env && *env){
        return atoi(env);
    }
    return DEFAULT_LEVEL;
}

// A non-negative number from the environment, or def when unset or invalid
static long long env_number(const char *name, long long def){
    const char *env = getenv(name);
    if (!env || !*env){
        return def;
    }
    char *end;
    long long value = strtoll(env, &end, 10);
    return *end || value < 0 ? def : value;
}

// Only gz writes to files matching a ZSTD_FILE_TARGETS glob become zstd (unset:
// none), so the intermediates can be converted while the final outputs stay
// gzip. Like BUFFER_FILE_TARGETS, a pattern with a / matches the full path,
// otherwise the basename
static int is_target(const char *path){
    const char *env = getenv("ZSTD_FILE_TARGETS");
    if (!env || !*env){
        return 0;
    }

    char *list = strdup(env);
    if (!list){
        return 0;
    }
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;

    int match = 0;
    char *save;
    for (char *p = strtok_r(list, ",; \t\n", &save); p && !match; p = strtok_r(NULL, ",; \t\n", &save)){
        match = fnmatch(p, strchr(p, '/') ? path : base, 0) == 0;
    }
    free(list);
    return match;
}

// gzopen's mode as zlib reads it: the last of r, w and a wins. 0 for what
// only zlib handles: "+", no r/w/a, or a transparent ("T") write
static int open_mode(const char *mode){
    int m = 0;
    int transparent = 0;
    for (const char *p = mode; *p; p++){
        if (*p == 'r' || *p == 'w' || *p == 'a'){
            m = *p;
        } else if (*p == '+'){
            return 0;
        } else if (*p == 'T'){
            transparent = 1;
        }
    }
    return m == 'w' && transparent ? 0 : m;
}

static double now(void){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// The library's handle behind file, or NULL for one of zlib's. Processes
// with no zstd file open never take the lock
static zstd_handle_t *get_handle(gzFile file){
    if (!file || __atomic_load_n(&handle_count, __ATOMIC_ACQUIRE) == 0){
        return NULL;
    }
    zstd_handle_t *found = NULL;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < handle_slots && !found; i++){
        if ((gzFile)handles[i] == file)
            found = handles[i];
    }
    pthread_mutex_unlock(&lock);
    return found;
}

static int write_all(int fd, const void *buf, size_t len){
    const unsigned char *ptr = buf;
    while (len > 0){
        ssize_t written = write(fd, ptr, len);
        if (written < 0){
            if (errno == EINTR)
                continue;
            return -1;
        }
        ptr += written;
        len -= written;
    }
    return 0;
}

// 1 if the 4 bytes at offset are the zstd magic number, 0 otherwise (gzip,
// plain, too short)
static int is_zstd_at(int fd, off_t offset){
    unsigned char magic[4];
    if (pread(fd, magic, 4, offset) != 4){
        return 0;
    }
    // ZSTD_MAGICNUMBER is stored little-endian at the start of every frame
    return (magic[0] | magic[1] << 8 | magic[2] << 16 | (unsigned)magic[3] << 24) == ZSTD_MAGICNUMBER;
}

// Records the error gzerror reports, printing it the first time
static void set_error(zstd_handle_t *h, int err, const char *why){
    if (h->err == Z_OK){
        fprintf(stderr, "zstd_lib: can't %s %s: %s\n", h->mode == WRITE_STREAM ? "write" : "read", h->path, why);
    }
    h->err = err;
    free(h->msg);
    h->msg = NULL;
    if (err != Z_MEM_ERROR && asprintf(&h->msg, "%s: %s", h->path, why) < 0){
        h->msg = NULL;
    }
}

// in and out are allocated at the first I/O, so gzbuffer can still size them
static int alloc_buffers(zstd_handle_t *h){
    if (h->out){
        return 0;
    }
    h->out = malloc(h->buf_size);
    h->in = h->mode == READ_STREAM ? malloc(h->buf_size) : NULL;
    if (!h->out || (h->mode == READ_STREAM && !h->in)){
        free(h->out);
        free(h->in);
        h->out = h->in = NULL;
        set_error(h, Z_MEM_ERROR, "out of memory");
        return -1;
    }
    return 0;
}


/* Contexts */

// Contexts are reset and reused across files instead of recreated, which
// keeps their allocated tables. Creating, resetting and freeing happen
// outside the lock
static ZSTD_CCtx *take_cctx(void){
    ZSTD_CCtx *cctx = NULL;
    pthread_mutex_lock(&lock);
    if (cctx_pooled > 0){
        cctx = cctx_pool[--cctx_pooled];
    }
    pthread_mutex_unlock(&lock);
    return cctx ? cctx : ZSTD_createCCtx();
}

static void put_cctx(ZSTD_CCtx *cctx){
    ZSTD_CCtx_reset(cctx, ZSTD_reset_session_and_parameters);
    pthread_mutex_lock(&lock);
    int kept = cctx_pooled < POOL_SIZE;
    if (kept){
        cctx_pool[cctx_pooled++] = cctx;
    }
    pthread_mutex_unlock(&lock);
    if (!kept){
        ZSTD_freeCCtx(cctx);
    }
}

static ZSTD_DCtx *take_dctx(void){
    ZSTD_DCtx *dctx = NULL;
    pthread_mutex_lock(&lock);
    if (dctx_pooled > 0){
        dctx = dctx_pool[--dctx_pooled];
    }
    pthread_mutex_unlock(&lock);
    return dctx ? dctx : ZSTD_createDCtx();
}

static void put_dctx(ZSTD_DCtx *dctx){
    ZSTD_DCtx_reset(dctx, ZSTD_reset_session_and_parameters);
    pthread_mutex_lock(&lock);
    int kept = dctx_pooled < POOL_SIZE;
    if (kept){
        dctx_pool[dctx_pooled++] = dctx;
    }
    pthread_mutex_unlock(&lock);
    if (!kept){
        ZSTD_freeDCtx(dctx);
    }
}


/* Handles */

// NULL only when out of memory: the table grows as needed
static zstd_handle_t *add_handle(int mode, int fd, const char *path){
    zstd_handle_t *h = calloc(1, sizeof(zstd_handle_t));
    if (!h || !(h->path = strdup(path))){
        free(h);
        return NULL;
    }
    h->mode = mode;
    h->fd = fd;
    h->err = Z_OK;
    h->buf_size = DEFAULT_BUFFER_SIZE;

    pthread_mutex_lock(&lock);
    int slot = 0;
    while (slot < handle_slots && handles[slot]){
        slot++;
    }
    if (slot == handle_slots){
        int slots = handle_slots ? handle_slots * 2 : INITIAL_HANDLES;
        zstd_handle_t **grown = realloc(handles, slots * sizeof(*handles));
        if (!grown){
            pthread_mutex_unlock(&lock);
            free(h->path);
            free(h);
            return NULL;
        }
        memset(grown + handle_slots, 0, (slots - handle_slots) * sizeof(*grown));
        handles = grown;
        handle_slots = slots;
    }
    handles[slot] = h;
    __atomic_add_fetch(&handle_count, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&lock);
    return h;
}

static void free_handle(zstd_handle_t *h){
    pthread_mutex_lock(&lock);
    for (int i = 0; i < handle_slots; i++){
        if (handles[i] == h){
            handles[i] = NULL;
            __atomic_sub_fetch(&handle_count, 1, __ATOMIC_RELEASE);
        }
    }
    pthread_mutex_unlock(&lock);
    free(h->in);
    free(h->out);
    free(h->cache);
    free(h->path);
    free(h->msg);
    free(h);
}


/* Open */

static gzFile open_write(const char *path, const char *mode){
    // Same flags as zlib: "x" for O_EXCL, "e" for O_CLOEXEC
    int flags = O_WRONLY | O_CREAT | O_TRUNC | (strchr(mode, 'x') ? O_EXCL : 0) | (strchr(mode, 'e') ? O_CLOEXEC : 0);
    int fd = open(path, flags, 0666);
    if (fd < 0){
        return NULL;
    }
    ZSTD_CCtx *cctx = take_cctx();
    zstd_handle_t *h = cctx ? add_handle(WRITE_STREAM, fd, path) : NULL;
    if (!h){
        if (cctx) put_cctx(cctx);
        close(fd);
        errno = ENOMEM;
        return NULL;
    }
    h->cctx = cctx;
    h->level = zstd_level();

    // No workers: the frame is compressed on the caller's thread, like zlib.
    // The checksum (XXH64 of the content, checked by every decoder) catches
    // damaged data, as gzip's CRC-32 does. ZSTD_WINDOW_LOG caps the window,
    // which bounds the writer's and every reader's memory (0: the level's
    // default)
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, h->level);
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_nbWorkers, 0);
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 1);
    long long window_log = env_number("ZSTD_WINDOW_LOG", 0);
    if (window_log){
        size_t ret = ZSTD_CCtx_setParameter(cctx, ZSTD_c_windowLog, (int)window_log);
        if (ZSTD_isError(ret)){
            fprintf(stderr, "zstd_lib: ZSTD_WINDOW_LOG=%lld ignored: %s\n", window_log, ZSTD_getErrorName(ret));
        }
    }
    return (gzFile)h;
}

// Takes over fd, whose first frame starts at start. On failure the caller
// still owns fd
static gzFile open_read(int fd, off_t start, const char *label){
    ZSTD_DCtx *dctx = take_dctx();
    zstd_handle_t *h = dctx ? add_handle(READ_STREAM, fd, label) : NULL;
    if (!h){
        fprintf(stderr, "zstd_lib: can't read %s: out of memory\n", label);
        if (dctx) put_dctx(dctx);
        errno = ENOMEM;
        return NULL;
    }
    h->dctx = dctx;
    h->start = start;
    h->cache_limit = env_number("ZSTD_HEADER_CACHE", DEFAULT_HEADER_CACHE);

    // Frames written with a ZSTD_WINDOW_LOG above the decoder's default limit
    // can only be read with the same setting
    long long window_log = env_number("ZSTD_WINDOW_LOG", 0);
    if (window_log > WINDOW_LOG_LIMIT_DEFAULT){
        ZSTD_DCtx_setParameter(dctx, ZSTD_d_windowLogMax, (int)window_log);
    }
    return (gzFile)h;
}

// Any regular file that starts with the zstd magic number is read here,
// whatever the targets, so a zstd file is always read correctly. gzip and
// plain files go on to the next zlib
static gzFile open_read_path(const char *path, const char *mode, original_gzopen_t original_gzopen){
    int fd = open(path, O_RDONLY | (strchr(mode, 'e') ? O_CLOEXEC : 0));
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || !is_zstd_at(fd, 0)){
        if (fd >= 0) close(fd);
        return original_gzopen(path, mode);
    }
    gzFile file = open_read(fd, 0, path);
    if (!file){
        close(fd);
    }
    return file;
}

// Shared by gzopen and gzopen64; appends and non-target writes stay with zlib
static gzFile open_gz(const char *name, const char *path, const char *mode, original_gzopen_t original_gzopen){
    gzFile file;
    int m = open_mode(mode);
    if (m == 'w' && is_target(path)){
        file = open_write(path, mode);
    } else if (m == 'r'){
        file = open_read_path(path, mode, original_gzopen);
    } else{
        file = original_gzopen(path, mode);
    }

    if (debug_enabled()){
        fprintf(stderr, "%s: %s, mode %s -> %p%s\n", name, path, mode, (void*)file,
            get_handle(file) ? " (zstd)" : "");
    }
    return file;
}

gzFile gzopen(const char *path, const char *mode) {
    static original_gzopen_t original_gzopen = NULL;
    if (!original_gzopen) original_gzopen = dlsym(RTLD_NEXT, "gzopen");

    return open_gz("gzopen", path, mode, original_gzopen);
}

gzFile gzopen64(const char *path, const char *mode) {
    static original_gzopen_t original_gzopen64 = NULL;
    if (!original_gzopen64) original_gzopen64 = dlsym(RTLD_NEXT, "gzopen64");

    return open_gz("gzopen64", path, mode, original_gzopen64);
}

// Writes stay with zlib: there's no path to match against the targets. A
// regular file open for reading is checked for zstd from its current offset
gzFile gzdopen(int fd, const char *mode) {
    static original_gzdopen_t original_gzdopen = NULL;
    if (!original_gzdopen) original_gzdopen = dlsym(RTLD_NEXT, "gzdopen");

    gzFile file;
    struct stat st;
    off_t start = -1;
    if (open_mode(mode) == 'r' && fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
            (start = lseek(fd, 0, SEEK_CUR)) >= 0 && is_zstd_at(fd, start)){
        char label[32];
        snprintf(label, sizeof(label), "<fd:%d>", fd);
        file = open_read(fd, start, label);
    } else{
        file = original_gzdopen(fd, mode);
    }

    if (debug_enabled()){
        fprintf(stderr, "gzdopen: fd %d, mode %s -> %p%s\n", fd, mode, (void*)file,
            get_handle(file) ? " (zstd)" : "");
    }
    return file;
}

// Like zlib: only before the first I/O. Sets both buffers, so the size of
// every write() or read()
int gzbuffer(gzFile file, unsigned size) {
    static original_gzbuffer_t original_gzbuffer = NULL;
    if (!original_gzbuffer) original_gzbuffer = dlsym(RTLD_NEXT, "gzbuffer");

    zstd_handle_t *h = get_handle(file);
    if (!h){
        return original_gzbuffer(file, size);
    }
    if (h->out){
        return -1;
    }
    h->buf_size = size < 8 ? 8 : size;
    return 0;
}

// The level comes from ZSTD_LEVEL, as for gzopen's mode string
int gzsetparams(gzFile file, int level, int strategy) {
    static original_gzsetparams_t original_gzsetparams = NULL;
    if (!original_gzsetparams) original_gzsetparams = dlsym(RTLD_NEXT, "gzsetparams");

    zstd_handle_t *h = get_handle(file);
    if (!h){
        return original_gzsetparams(file, level, strategy);
    }
    return h->mode == WRITE_STREAM && h->err == Z_OK ? Z_OK : Z_STREAM_ERROR;
}


/* Write */

static int write_out(zstd_handle_t *h){
    if (write_all(h->fd, h->out, h->out_pos) < 0){
        set_error(h, Z_ERRNO, strerror(errno));
        return -1;
    }
    h->raw_bytes += h->out_pos;
    h->out_pos = 0;
    return 0;
}

// Compresses len bytes straight from buf. out goes to the file each time it
// fills, so every write() is buf_size bytes; ZSTD_e_flush and ZSTD_e_end
// also write out whatever is left. Returns 0 or -1
static int compress_data(zstd_handle_t *h, const void *buf, size_t len, ZSTD_EndDirective op){
    if (h->err != Z_OK || alloc_buffers(h) < 0){
        return -1;
    }

    double start = now();
    ZSTD_inBuffer in = {buf, len, 0};
    for (;;){
        ZSTD_outBuffer out = {h->out, h->buf_size, h->out_pos};
        size_t left = ZSTD_compressStream2(h->cctx, &out, &in, op);
        h->out_pos = out.pos;
        if (ZSTD_isError(left)){
            set_error(h, Z_STREAM_ERROR, ZSTD_getErrorName(left));
            return -1;
        }
        // A flush or end is done once zstd holds nothing back (left is 0)
        int done = op == ZSTD_e_continue ? in.pos == in.size : left == 0;
        if (h->out_pos == h->buf_size || (done && op != ZSTD_e_continue && h->out_pos > 0)){
            if (write_out(h) < 0)
                return -1;
        }
        if (done)
            break;
    }
    h->upos += len;
    h->data_bytes += len;
    h->secs += now() - start;
    return 0;
}

int gzwrite(gzFile file, const void *buf, unsigned int len) {
    static original_gzwrite_t original_gzwrite = NULL;
    if (!original_gzwrite) original_gzwrite = dlsym(RTLD_NEXT, "gzwrite");

    if (debug_enabled()){
        fprintf(stderr, "gzwrite: %p, %u bytes\n", (void*)file, len);
    }

    zstd_handle_t *h = get_handle(file);
    if (!h){
        return original_gzwrite(file, buf, len);
    }
    // Like zlib, 0 on errors
    if (h->mode != WRITE_STREAM){
        return 0;
    }
    if (len > INT_MAX){
        set_error(h, Z_DATA_ERROR, "requested length does not fit in int");
        return 0;
    }
    return compress_data(h, buf, len, ZSTD_e_continue) < 0 ? 0 : (int)len;
}

z_size_t gzfwrite(voidpc buf, z_size_t size, z_size_t nitems, gzFile file) {
    static original_gzfwrite_t original_gzfwrite = NULL;
    if (!original_gzfwrite) original_gzfwrite = dlsym(RTLD_NEXT, "gzfwrite");

    zstd_handle_t *h = get_handle(file);
    if (!h){
        return original_gzfwrite(buf, size, nitems, file);
    }
    if (h->mode != WRITE_STREAM){
        return 0;
    }
    z_size_t len = nitems * size;
    if (size && len / size != nitems){
        set_error(h, Z_STREAM_ERROR, "request does not fit in a size_t");
        return 0;
    }
    if (len == 0){
        return 0;
    }
    return compress_data(h, buf, len, ZSTD_e_continue) < 0 ? 0 : nitems;
}

int gzputc(gzFile file, int c) {
    static original_gzputc_t original_gzputc = NULL;
    if (!original_gzputc) original_gzputc = dlsym(RTLD_NEXT, "gzputc");

    zstd_handle_t *h = get_handle(file);
    if (!h){
        return original_gzputc(file, c);
    }
    unsigned char byte = c;
    if (h->mode != WRITE_STREAM || compress_data(h, &byte, 1, ZSTD_e_continue) < 0){
        return -1;
    }
    return byte;
}

int gzputs(gzFile file, const char *s) {
    static original_gzputs_t original_gzputs = NULL;
    if (!original_gzputs) original_gzputs = dlsym(RTLD_NEXT, "gzputs");

    zstd_handle_t *h = get_handle(file);
    if (!h){
        return original_gzputs(file, s);
    }
    if (h->mode != WRITE_STREAM){
        return -1;
    }
    size_t len = strlen(s);
    if (len > INT_MAX){
        set_error(h, Z_STREAM_ERROR, "string length does not fit in int");
        return -1;
    }
    return compress_data(h, s, len, ZSTD_e_continue) < 0 ? -1 : (int)len;
}

// No length limit, unlike zlib's, which fails past its buffer size. Returns
// the bytes written, or a negative zlib error
static int printf_handle(zstd_handle_t *h, const char *format, va_list va){
    if (h->mode != WRITE_STREAM || h->err != Z_OK){
        return Z_STREAM_ERROR;
    }

    char stack[8192];
    va_list copy;
    va_copy(copy, va);
    int len = vsnprintf(stack, sizeof(stack), format, copy);
    va_end(copy);
    if (len <= 0){
        return 0;
    }

    char *text = stack;
    if ((size_t)len >= sizeof(stack)){
        text = malloc((size_t)len + 1);
        if (!text){
            set_error(h, Z_MEM_ERROR, "out of memory");
            return Z_MEM_ERROR;
        }
        vsnprintf(text, (size_t)len + 1, format, va);
    }
    int ret = compress_data(h, text, len, ZSTD_e_continue) < 0 ? h->err : len;
    if (text != stack){
        free(text);
    }
    return ret;
}

int gzvprintf(gzFile file, const char *format, va_list va) {
    static original_gzvprintf_t original_gzvprintf = NULL;
    if (!original_gzvprintf) original_gzvprintf = dlsym(RTLD_NEXT, "gzvprintf");

    zstd_handle_t *h = get_handle(file);
    return h ? printf_handle(h, format, va) : original_gzvprintf(file, format, va);
}

// A variadic call can't be passed on, so zlib's handles go to its gzvprintf
int gzprintf(gzFile file, const char *format, ...) {
    static original_gzvprintf_t original_gzvprintf = NULL;
    if (!original_gzvprintf) original_gzvprintf = dlsym(RTLD_NEXT, "gzvprintf");

    va_list va;
    va_start(va, format);
    zstd_handle_t *h = get_handle(file);
    int ret = h ? printf_handle(h, format, va) : original_gzvprintf(file, format, va);
    va_end(va);
    return ret;
}

// Z_FINISH ends the frame (the next write starts a new one, as zlib starts a
// new gzip member); the other flushes write out everything so far, so a
// reader can decode it before gzclose
int gzflush(gzFile file, int flush) {
    static original_gzflush_t original_gzflush = NULL;
    if (!original_gzflush) original_gzflush = dlsym(RTLD_NEXT, "gzflush");

    zstd_handle_t *h = get_handle(file);
    if (!h){
        return original_gzflush(file, flush);
    }
    if (h->mode != WRITE_STREAM || h->err != Z_OK || flush < Z_NO_FLUSH || flush > Z_FINISH){
        return Z_STREAM_ERROR;
    }
    if (flush == Z_NO_FLUSH){
        return Z_OK;
    }
    ZSTD_EndDirective op = flush == Z_FINISH ? ZSTD_e_end : ZSTD_e_flush;
    return compress_data(h, NULL, 0, op) < 0 ? h->err : Z_OK;
}


/* Read */

// Keeps the bytes just decoded at dpos that fall in the first cache_limit
// bytes of the file
static void cache_add(zstd_handle_t *h, const unsigned char *data, size_t n){
    z_off64_t cached = h->cache_len;
    if (h->cache_len >= h->cache_limit || h->dpos > cached || h->dpos + (z_off64_t)n <= cached){
        return;
    }
    if (!h->cache){
        // Pages are only touched as the cache fills
        h->cache = malloc(h->cache_limit);
        if (!h->cache){
            h->cache_limit = 0;
            return;
        }
    }
    size_t from = cached - h->dpos;
    size_t len = n - from;
    if (len > h->cache_limit - h->cache_len){
        len = h->cache_limit - h->cache_len;
    }
    memcpy(h->cache + h->cache_len, data + from, len);
    h->cache_len += len;
}

// Decodes up to cap bytes into dst from dpos on, reading the file buf_size
// bytes at a time. Returns how many (fewer only at the end of the data), 0 at
// the end, -1 on error. A file that ends inside a frame (cut short, or never
// closed by gzclose) is an error once the bytes before the cut are returned,
// never a quiet end of file
static ssize_t decode(zstd_handle_t *h, unsigned char *dst, size_t cap){
    double start = now();
    ZSTD_outBuffer out = {dst, cap, 0};
    while (out.pos < out.size && !h->end){
        if (h->in_pos == h->in_end){
            if (h->raw_eof){
                if (!h->frame_open){
                    h->end = 1;
                } else if (out.pos == 0){
                    set_error(h, Z_DATA_ERROR, "unexpected end of file");
                    return -1;
                }
                break;
            }
            ssize_t got = read(h->fd, h->in, h->buf_size);
            if (got < 0){
                if (errno == EINTR)
                    continue;
                set_error(h, Z_ERRNO, strerror(errno));
                return -1;
            }
            h->raw_eof = got == 0;
            h->in_pos = 0;
            h->in_end = got;
            h->raw_bytes += got;
            continue;
        }

        ZSTD_inBuffer in = {h->in, h->in_end, h->in_pos};
        size_t hint = ZSTD_decompressStream(h->dctx, &out, &in);
        h->in_pos = in.pos;
        if (ZSTD_isError(hint)){
            set_error(h, Z_DATA_ERROR, ZSTD_getErrorName(hint));
            return -1;
        }
        // 0 once a frame is decoded and flushed; another frame may follow
        h->frame_open = hint != 0;
    }

    cache_add(h, dst, out.pos);
    h->dpos += out.pos;
    h->data_bytes += out.pos;
    h->secs += now() - start;
    return out.pos;
}

// Back to the first frame, for a read behind the decoder that the cache
// doesn't cover. Costs decoding the file again up to the target
static int restart(zstd_handle_t *h){
    if (lseek(h->fd, h->start, SEEK_SET) < 0){
        set_error(h, Z_ERRNO, strerror(errno));
        return -1;
    }
    ZSTD_DCtx_reset(h->dctx, ZSTD_reset_session_only);
    h->in_pos = h->in_end = 0;
    h->raw_eof = h->frame_open = h->end = 0;
    h->dpos = 0;
    h->out_len = 0;
    h->rewinds++;
    return 0;
}

// Points *ptr at the bytes from upos on, in the cache or in out, decoding
// more when needed (and skipping ahead after a forward seek). Returns how
// many there are, 0 at the end of the data, -1 on error
static ssize_t peek(zstd_handle_t *h, const unsigned char **ptr){
    if (h->err != Z_OK || alloc_buffers(h) < 0){
        return -1;
    }
    for (;;){
        if (h->upos < (z_off64_t)h->cache_len){
            *ptr = h->cache + h->upos;
            return h->cache_len - h->upos;
        }
        // out holds the bytes from dpos - out_len to dpos, so a short seek
        // back stays in it; anything before means starting over
        if (h->upos < h->dpos - (z_off64_t)h->out_len && restart(h) < 0){
            return -1;
        }
        if (h->upos < h->dpos){
            *ptr = h->out + h->out_len - (h->dpos - h->upos);
            return h->dpos - h->upos;
        }
        if (h->end){
            // Like zlib, a seek past the end leaves the position at the end
            h->upos = h->dpos;
            return 0;
        }
        ssize_t n = decode(h, h->out, h->buf_size);
        if (n < 0){
            return -1;
        }
        // At the end nothing was written, and out still ends at dpos
        if (n > 0){
            h->out_len = n;
        }
    }
}

// Copies up to len bytes from upos into buf; fewer only at the end. Large
// reads with nothing decoded ahead go straight into buf. Returns the count,
// or -1 (dropping what was copied, as zlib does)
static ssize_t read_data(zstd_handle_t *h, unsigned char *buf, size_t len){
    if (h->err != Z_OK || alloc_buffers(h) < 0){
        return -1;
    }
    size_t got = 0;
    while (got < len){
        if (h->nback > 0){
            buf[got++] = h->back[--h->nback];
            h->upos++;
            continue;
        }
        if (h->upos >= (z_off64_t)h->cache_len && h->upos == h->dpos && !h->end && len - got >= h->buf_size){
            // out no longer ends at dpos once the decoder writes elsewhere
            h->out_len = 0;
            ssize_t n = decode(h, buf + got, len - got);
            if (n < 0)
                return -1;
            got += n;
            h->upos += n;
            continue;
        }

        const unsigned char *src;
        ssize_t n = peek(h, &src);
        if (n < 0){
            return -1;
        }
        if (n == 0){
            h->past = 1;
            break;
        }
        if ((size_t)n > len - got){
            n = len - got;
        }
        memcpy(buf + got, src, n);
        got += n;
        h->upos += n;
    }
    return got;
}

int gzread(gzFile file, voidp buf, unsigned len) {
    static original_gzread_t original_gzread = NULL;
    if (!original_gzread) original_gzread = dlsym(RTLD_NEXT, "gzread");

    int got;
    zstd_handle_t *h = get_handle(file);
    if (!h){
        got = original_gzread(file, buf, len);
    } else if (h->mode != READ_STREAM){
        got = -1;
    } else if (len > INT_MAX){
        set_error(h, Z_STREAM_ERROR, "request does not fit in an int");
        got = -1;
    } else{
        got = read_data(h, buf, len);
    }

    if (debug_enabled()){
        fprintf(stderr, "gzread: %p, asked %u, got %d\n", (void*)file, len, got);
    }
    return got;
}

z_size_t gzfread(voidp buf, z_size_t size, z_size_t nitems, gzFile file) {
    static original_gzfread_t original_gzfread = NULL;
    if (!original_gzfread) original_gzfread = dlsym(RTLD_NEXT, "gzfread");

    zstd_handle_t *h = get_handle(file);
    if (!h){
        return original_gzfread(buf, size, nitems, file);
    }
    if (h->mode != READ_STREAM){
        return 0;
    }
    z_size_t len = nitems * size;
    if (size && len / size != nitems){
        set_error(h, Z_STREAM_ERROR, "request does not fit in a size_t");
        return 0;
    }
    ssize_t got = len ? read_data(h, buf, len) : 0;
    return got > 0 ? (z_size_t)got / size : 0;
}

// Like zlib, -1 at the end of the data or on an error
static int getc_handle(gzFile file, original_gzgetc_t original){
    zstd_handle_t *h = get_handle(file);
    if (!h){
        return original(file);
    }
    if (h->mode != READ_STREAM){
        return -1;
    }
    if (h->nback > 0){
        h->upos++;
        return h->back[--h->nback];
    }
    const unsigned char *src;
    ssize_t n = peek(h, &src);
    if (n <= 0){
        if (n == 0) h->past = 1;
        return -1;
    }
    h->upos++;
    return *src;
}

int gzgetc(gzFile file) {
    static original_gzgetc_t original_gzgetc = NULL;
    if (!original_gzgetc) original_gzgetc = dlsym(RTLD_NEXT, "gzgetc");

    return getc_handle(file, original_gzgetc);
}

// Older name for gzgetc, still exported by zlib
int gzgetc_(gzFile file) {
    static original_gzgetc_t original_gzgetc_ = NULL;
    if (!original_gzgetc_) original_gzgetc_ = dlsym(RTLD_NEXT, "gzgetc_");

    return getc_handle(file, original_gzgetc_);
}

// The character needn't be the one just read; it is returned by the next read
int gzungetc(int c, gzFile file) {
    static original_gzungetc_t original_gzungetc = NULL;
    if (!original_gzungetc) original_gzungetc = dlsym(RTLD_NEXT, "gzungetc");

    zstd_handle_t *h = get_handle(file);
    if (!h){
        return original_gzungetc(c, file);
    }
    if (h->mode != READ_STREAM || h->err != Z_OK || c < 0){
        return -1;
    }
    if (h->nback == UNGET_SIZE){
        set_error(h, Z_DATA_ERROR, "out of room to push characters");
        return -1;
    }
    h->back[h->nback++] = (unsigned char)c;
    h->upos--;
    h->past = 0;
    return c;
}

// As zlib's: reads up to len - 1 bytes, stopping after a newline, and NUL
// terminates them. NULL at the end of the data with nothing read, or on error
char *gzgets(gzFile file, char *buf, int len) {
    static original_gzgets_t original_gzgets = NULL;
    if (!original_gzgets) original_gzgets = dlsym(RTLD_NEXT, "gzgets");

    zstd_handle_t *h = get_handle(file);
    if (!h){
        return original_gzgets(file, buf, len);
    }
    if (h->mode != READ_STREAM || h->err != Z_OK || !buf || len < 1){
        return NULL;
    }

    char *str = buf;
    size_t left = (size_t)len - 1;
    while (left > 0){
        if (h->nback > 0){
            char c = h->back[--h->nback];
            h->upos++;
            *buf++ = c;
            left--;
            if (c == '\n')
                break;
            continue;
        }
        const unsigned char *src;
        ssize_t n = peek(h, &src);
        if (n < 0){
            return NULL;
        }
        if (n == 0){
            h->past = 1;
            break;
        }
        if ((size_t)n > left){
            n = left;
        }
        const unsigned char *eol = memchr(src, '\n', n);
        if (eol){
            n = eol - src + 1;
        }
        memcpy(buf, src, n);
        buf += n;
        left -= n;
        h->upos += n;
        if (eol)
            break;
    }
    if (buf == str){
        return NULL;
    }
    *buf = '\0';
    return str;
}


/* Seek and state */

// Same rules as zlib: no SEEK_END. Reads move anywhere from 0 on, lazily:
// the next read skips ahead, or restarts from the first frame when the target
// is behind both the decoder and the header cache. Writes only move forward,
// compressing zeros up to the new offset. Returns the new offset, or -1
static z_off64_t seek_handle(zstd_handle_t *h, z_off64_t offset, int whence){
    if (h->err != Z_OK || (whence != SEEK_SET && whence != SEEK_CUR)){
        return -1;
    }
    if (whence == SEEK_CUR){
        offset += h->upos;
    }
    if (offset < 0){
        return -1;
    }

    if (h->mode == WRITE_STREAM){
        static const unsigned char zeros[65536];
        if (offset < h->upos){
            return -1;
        }
        while (h->upos < offset){
            z_off64_t n = offset - h->upos;
            if (n > (z_off64_t)sizeof(zeros)) n = sizeof(zeros);
            if (compress_data(h, zeros, n, ZSTD_e_continue) < 0)
                return -1;
        }
    } else{
        h->upos = offset;
        h->nback = 0;
        h->past = 0;
    }
    return offset;
}

z_off_t gzseek(gzFile file, z_off_t offset, int whence) {
    static original_gzseek_t original_gzseek = NULL;
    if (!original_gzseek) original_gzseek = dlsym(RTLD_NEXT, "gzseek");

    zstd_handle_t *h = get_handle(file);
    z_off_t ret = h ? (z_off_t)seek_handle(h, offset, whence) : original_gzseek(file, offset, whence);

    if (debug_enabled()){
        fprintf(stderr, "gzseek: %p, %lld whence %d -> %lld\n", (void*)file, (long long)offset, whence, (long long)ret);
    }
    return ret;
}

z_off64_t gzseek64(gzFile file, z_off64_t offset, int whence) {
    static original_gzseek64_t original_gzseek64 = NULL;
    if (!original_gzseek64) original_gzseek64 = dlsym(RTLD_NEXT, "gzseek64");

    zstd_handle_t *h = get_handle(file);
    z_off64_t ret = h ? seek_handle(h, offset, whence) : original_gzseek64(file, offset, whence);

    if (debug_enabled()){
        fprintf(stderr, "gzseek64: %p, %lld whence %d -> %lld\n", (void*)file, (long long)offset, whence, (long long)ret);
    }
    return ret;
}

int gzrewind(gzFile file) {
    static original_gzrewind_t original_gzrewind = NULL;
    if (!original_gzrewind) original_gzrewind = dlsym(RTLD_NEXT, "gzrewind");

    zstd_handle_t *h = get_handle(file);
    if (!h){
        return original_gzrewind(file);
    }
    if (h->mode != READ_STREAM){
        return -1;
    }
    return seek_handle(h, 0, SEEK_SET) < 0 ? -1 : 0;
}

z_off_t gztell(gzFile file) {
    static original_gztell_t original_gztell = NULL;
    if (!original_gztell) original_gztell = dlsym(RTLD_NEXT, "gztell");

    zstd_handle_t *h = get_handle(file);
    return h ? (z_off_t)h->upos : original_gztell(file);
}

z_off64_t gztell64(gzFile file) {
    static original_gztell64_t original_gztell64 = NULL;
    if (!original_gztell64) original_gztell64 = dlsym(RTLD_NEXT, "gztell64");

    zstd_handle_t *h = get_handle(file);
    return h ? h->upos : original_gztell64(file);
}

// Compressed offset in the file, as zlib reports it: for reads, of the next
// byte the decoder will take
static z_off64_t offset_handle(zstd_handle_t *h){
    off_t pos = lseek(h->fd, 0, SEEK_CUR);
    if (pos < 0){
        return -1;
    }
    return h->mode == READ_STREAM ? pos - (off_t)(h->in_end - h->in_pos) : pos;
}

z_off_t gzoffset(gzFile file) {
    static original_gztell_t original_gzoffset = NULL;
    if (!original_gzoffset) original_gzoffset = dlsym(RTLD_NEXT, "gzoffset");

    zstd_handle_t *h = get_handle(file);
    return h ? (z_off_t)offset_handle(h) : original_gzoffset(file);
}

z_off64_t gzoffset64(gzFile file) {
    static original_gztell64_t original_gzoffset64 = NULL;
    if (!original_gzoffset64) original_gzoffset64 = dlsym(RTLD_NEXT, "gzoffset64");

    zstd_handle_t *h = get_handle(file);
    return h ? offset_handle(h) : original_gzoffset64(file);
}

// Like zlib, set only once a read has asked for more than was left
int gzeof(gzFile file) {
    static original_gzeof_t original_gzeof = NULL;
    if (!original_gzeof) original_gzeof = dlsym(RTLD_NEXT, "gzeof");

    zstd_handle_t *h = get_handle(file);
    if (!h){
        return original_gzeof(file);
    }
    return h->mode == READ_STREAM ? h->past : 0;
}

// The library's handles always decompress
int gzdirect(gzFile file) {
    static original_gzdirect_t original_gzdirect = NULL;
    if (!original_gzdirect) original_gzdirect = dlsym(RTLD_NEXT, "gzdirect");

    zstd_handle_t *h = get_handle(file);
    return h ? 0 : original_gzdirect(file);
}

const char *gzerror(gzFile file, int *errnum) {
    static original_gzerror_t original_gzerror = NULL;
    if (!original_gzerror) original_gzerror = dlsym(RTLD_NEXT, "gzerror");

    zstd_handle_t *h = get_handle(file);
    if (!h){
        return original_gzerror(file, errnum);
    }
    if (errnum){
        *errnum = h->err;
    }
    if (h->forked){
        return "zstd_lib: handle opened before fork() belongs to the parent";
    }
    if (h->err == Z_MEM_ERROR){
        return "out of memory";
    }
    return h->msg ? h->msg : "";
}

// As in zlib, a reader also forgets it reached the end of the file, so it
// can go on reading a file that has grown (e.g. after the writer's gzflush).
// A handle inherited across fork() stays unusable
void gzclearerr(gzFile file) {
    static original_gzclearerr_t original_gzclearerr = NULL;
    if (!original_gzclearerr) original_gzclearerr = dlsym(RTLD_NEXT, "gzclearerr");

    zstd_handle_t *h = get_handle(file);
    if (!h){
        original_gzclearerr(file);
        return;
    }
    if (h->forked){
        return;
    }
    if (h->mode == READ_STREAM){
        h->past = 0;
        h->raw_eof = 0;
        h->end = 0;
    }
    h->err = Z_OK;
    free(h->msg);
    h->msg = NULL;
}


/* Close */

// Ends the frame and closes the file; one summary line per file, always
// printed (scripts/lib/common.sh sums them). A handle inherited across fork()
// is only dropped: its descriptor copy is closed, nothing is written
static int close_handle(zstd_handle_t *h){
    int ret = h->err;
    if (h->forked){
        ret = Z_OK;
        if (h->mode == WRITE_STREAM) put_cctx(h->cctx);
        else put_dctx(h->dctx);
    } else if (h->mode == WRITE_STREAM){
        if (ret == Z_OK && compress_data(h, NULL, 0, ZSTD_e_end) < 0){
            ret = h->err;
        }
        if (ret == Z_OK){
            fprintf(stderr, "zstd_lib: level %d, %lld -> %lld bytes, %.2f s\n", h->level, h->data_bytes, h->raw_bytes, h->secs);
        }
        put_cctx(h->cctx);
    } else{
        fprintf(stderr, "zstd_lib: read, %lld -> %lld bytes, %d rewinds, %.2f s\n", h->raw_bytes, h->data_bytes, h->rewinds, h->secs);
        put_dctx(h->dctx);
    }
    if (close(h->fd) < 0 && ret == Z_OK){
        ret = Z_ERRNO;
    }
    free_handle(h);
    return ret;
}

int gzclose(gzFile file) {
    static original_gzclose_t original_gzclose = NULL;
    if (!original_gzclose) original_gzclose = dlsym(RTLD_NEXT, "gzclose");

    if (debug_enabled()){
        fprintf(stderr, "gzclose: %p\n", (void*)file);
    }

    zstd_handle_t *h = get_handle(file);
    return h ? close_handle(h) : original_gzclose(file);
}

int gzclose_r(gzFile file) {
    static original_gzclose_t original_gzclose_r = NULL;
    if (!original_gzclose_r) original_gzclose_r = dlsym(RTLD_NEXT, "gzclose_r");

    zstd_handle_t *h = get_handle(file);
    if (!h){
        return original_gzclose_r(file);
    }
    return h->mode == READ_STREAM ? close_handle(h) : Z_STREAM_ERROR;
}

int gzclose_w(gzFile file) {
    static original_gzclose_t original_gzclose_w = NULL;
    if (!original_gzclose_w) original_gzclose_w = dlsym(RTLD_NEXT, "gzclose_w");

    zstd_handle_t *h = get_handle(file);
    if (!h){
        return original_gzclose_w(file);
    }
    return h->mode == WRITE_STREAM ? close_handle(h) : Z_STREAM_ERROR;
}


/* Fork */

// The prepare handler holds the lock across fork(), so the child's copy of
// the table is consistent and its lock isn't held by a thread it doesn't have
static void fork_prepare(void){
    pthread_mutex_lock(&lock);
}

static void fork_parent(void){
    pthread_mutex_unlock(&lock);
}

// The child's handles are copies of the parent's and share its open files:
// writing through one would add data, or a frame end at gzclose, to the
// parent's stream, and reading would move the parent's file offset. So
// they are only marked: every call on them fails, and gzclose drops them
// without any I/O. Nothing else is allocated or printed here
static void fork_child(void){
    for (int i = 0; i < handle_slots; i++){
        if (handles[i]){
            handles[i]->forked = 1;
            handles[i]->err = Z_STREAM_ERROR;
        }
    }
    pthread_mutex_unlock(&lock);
}

__attribute__((constructor)) static void init(void){
    debug = getenv("ZSTD_DEBUG") != NULL;
    pthread_atfork(fork_prepare, fork_parent, fork_child);
}
