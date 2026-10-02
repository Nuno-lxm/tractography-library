#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
#include <fnmatch.h>
#include <time.h>
#include <sys/stat.h>
#include <zlib.h>
#include <zstd.h>

// zlib.h makes gzgetc a macro that reads the handle's fields directly; the
// library defines the real function instead
#undef gzgetc

#define MAX_HANDLES 64


typedef gzFile (*original_gzopen_t)(const char *path, const char *mode);
typedef int (*original_gzwrite_t)(gzFile file, const void *buf, unsigned int len);
typedef int (*original_gzread_t)(gzFile file, voidp buf, unsigned len);
typedef int (*original_gzclose_t)(gzFile file);
typedef z_off_t (*original_gzseek_t)(gzFile file, z_off_t offset, int whence);
typedef z_off64_t (*original_gzseek64_t)(gzFile file, z_off64_t offset, int whence);
typedef z_off_t (*original_gztell_t)(gzFile file);
typedef z_off64_t (*original_gztell64_t)(gzFile file);
typedef int (*original_gzeof_t)(gzFile file);
typedef const char *(*original_gzerror_t)(gzFile file, int *errnum);
typedef int (*original_gzgetc_t)(gzFile file);


// A gz file taken over from zlib. Writing: everything is buffered in data and
// compressed with zstd in one go at gzclose. Reading (only files that are
// already zstd): the whole file is decompressed into data at gzopen.
// writing comes first: zlib's gzgetc macro reads the first field as its count
// of buffered bytes, and 0 sends a read handle to the gzgetc function
typedef struct{
    int writing;
    int eof;
    int fd;
    int level;
    unsigned char *data;
    size_t size;
    size_t capacity;
    size_t pos;
} zstd_handle_t;

static zstd_handle_t *handles[MAX_HANDLES];


/* Helpers */

// Per-call prints only with ZSTD_DEBUG set
static int debug_enabled(void){
    static int enabled = -1;
    if (enabled < 0){
        enabled = getenv("ZSTD_DEBUG") != NULL;
    }
    return enabled;
}

// ZSTD_LEVEL sets the level; the zlib level in the mode string is ignored
static int zstd_level(void){
    const char *env = getenv("ZSTD_LEVEL");
    if (env && *env){
        return atoi(env);
    }
    return ZSTD_CLEVEL_DEFAULT;
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

static double now(void){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int find_handle(gzFile file){
    for (int i = 0; i < MAX_HANDLES; i++){
        if (handles[i] && (gzFile)handles[i] == file)
            return i;
    }
    return -1;
}

static int free_slot(void){
    for (int i = 0; i < MAX_HANDLES; i++){
        if (!handles[i])
            return i;
    }
    return -1;
}

// Makes room for len more bytes in a write handle's buffer
static int grow(zstd_handle_t *h, size_t len){
    if (h->size + len <= h->capacity){
        return 0;
    }
    size_t new_capacity = h->capacity ? h->capacity : 65536;
    while (new_capacity < h->size + len){
        new_capacity *= 2;
    }
    unsigned char *new_data = realloc(h->data, new_capacity);
    if (!new_data){
        perror("realloc");
        return -1;
    }
    h->data = new_data;
    h->capacity = new_capacity;
    return 0;
}

static int write_all(int fd, const void *buf, size_t len){
    const unsigned char *ptr = buf;
    while (len > 0){
        ssize_t written = write(fd, ptr, len);
        if (written < 0){
            return -1;
        }
        ptr += written;
        len -= written;
    }
    return 0;
}

static int read_all(int fd, void *buf, size_t len){
    unsigned char *ptr = buf;
    while (len > 0){
        ssize_t got = read(fd, ptr, len);
        if (got <= 0){
            return -1;
        }
        ptr += got;
        len -= got;
    }
    return 0;
}

// 1 if path starts with the zstd magic number, 0 otherwise (gzip, plain,
// too short, missing)
static int is_zstd(const char *path){
    int fd = open(path, O_RDONLY);
    if (fd < 0){
        return 0;
    }
    unsigned char magic[4];
    int ok = read_all(fd, magic, 4) == 0;
    close(fd);

    // ZSTD_MAGICNUMBER is stored little-endian at the start of every frame
    return ok && (magic[0] | magic[1] << 8 | magic[2] << 16 | (unsigned)magic[3] << 24) == ZSTD_MAGICNUMBER;
}

// Decompresses a zstd file written by gzclose (one frame with its size
// stored). Returns NULL with errno set on any failure
static unsigned char *load_zstd(const char *path, size_t *out_size){
    int fd = open(path, O_RDONLY);
    if (fd < 0){
        return NULL;
    }

    unsigned char *in = NULL;
    unsigned char *out = NULL;
    const char *why = NULL;
    struct stat st;
    double start = now();

    if (fstat(fd, &st) < 0){
        why = strerror(errno);
        goto fail;
    }
    in = malloc(st.st_size);
    if (!in){
        why = "out of memory";
        goto fail;
    }
    if (read_all(fd, in, st.st_size) < 0){
        why = "read failed";
        goto fail;
    }

    unsigned long long size = ZSTD_getFrameContentSize(in, st.st_size);
    if (size == ZSTD_CONTENTSIZE_UNKNOWN || size == ZSTD_CONTENTSIZE_ERROR){
        why = "no content size in the frame header";
        goto fail;
    }

    // malloc(0) may return NULL, so always ask for at least one byte
    out = malloc(size ? size : 1);
    if (!out){
        why = "out of memory";
        goto fail;
    }
    // Extra frames after the first don't fit in size and fail here
    size_t got = ZSTD_decompress(out, size, in, st.st_size);
    if (ZSTD_isError(got) || got != size){
        why = ZSTD_isError(got) ? ZSTD_getErrorName(got) : "short frame";
        goto fail;
    }

    // One summary line per file read, always printed
    fprintf(stderr, "zstd_lib: read, %lld -> %llu bytes, %.2f s\n", (long long)st.st_size, size, now() - start);

    free(in);
    close(fd);
    *out_size = size;
    return out;

fail:
    fprintf(stderr, "zstd_lib: can't read %s: %s\n", path, why);
    free(in);
    free(out);
    close(fd);
    errno = EIO;
    return NULL;
}


/* Open */

static gzFile open_write(const char *path, const char *mode, original_gzopen_t original_gzopen){
    int slot = free_slot();
    if (slot < 0){
        return original_gzopen(path, mode);
    }

    zstd_handle_t *h = calloc(1, sizeof(zstd_handle_t));
    if (!h){
        return original_gzopen(path, mode);
    }

    h->fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (h->fd < 0){
        free(h);
        return NULL;
    }
    h->writing = 1;
    h->level = zstd_level();
    handles[slot] = h;
    return (gzFile)h;
}

// zlib would read a zstd file as uncompressed data without any error, so a
// zstd file this library can't serve fails the open instead
static gzFile open_read(const char *path, const char *mode, original_gzopen_t original_gzopen){
    if (!is_zstd(path)){
        return original_gzopen(path, mode);
    }

    int slot = free_slot();
    if (slot < 0){
        fprintf(stderr, "zstd_lib: can't read %s: all %d handles in use\n", path, MAX_HANDLES);
        errno = EMFILE;
        return NULL;
    }

    size_t size;
    unsigned char *data = load_zstd(path, &size);
    if (!data){
        return NULL;
    }

    zstd_handle_t *h = calloc(1, sizeof(zstd_handle_t));
    if (!h){
        fprintf(stderr, "zstd_lib: can't read %s: out of memory\n", path);
        free(data);
        errno = ENOMEM;
        return NULL;
    }
    h->fd = -1;
    h->data = data;
    h->size = size;
    handles[slot] = h;
    return (gzFile)h;
}

// Shared by gzopen and gzopen64; appends and non-target writes stay with zlib.
// Reads of any file that is zstd are always taken over
static gzFile open_gz(const char *name, const char *path, const char *mode, original_gzopen_t original_gzopen){
    gzFile file;
    if (strchr(mode, 'a') || (strchr(mode, 'w') && !is_target(path))){
        file = original_gzopen(path, mode);
    } else if (strchr(mode, 'w')){
        file = open_write(path, mode, original_gzopen);
    } else{
        file = open_read(path, mode, original_gzopen);
    }

    if (debug_enabled()){
        fprintf(stderr, "%s: %s, mode %s -> %p%s\n", name, path, mode, (void*)file,
            find_handle(file) >= 0 ? " (zstd)" : "");
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


/* Write */

int gzwrite(gzFile file, const void *buf, unsigned int len) {
    static original_gzwrite_t original_gzwrite = NULL;
    if (!original_gzwrite) original_gzwrite = dlsym(RTLD_NEXT, "gzwrite");

    if (debug_enabled()){
        fprintf(stderr, "gzwrite: %p, %u bytes\n", (void*)file, len);
    }

    int slot = find_handle(file);
    if (slot < 0){
        return original_gzwrite(file, buf, len);
    }

    zstd_handle_t *h = handles[slot];
    if (!h->writing){
        return 0;
    }

    if (grow(h, len) < 0){
        return 0; /* gzwrite returns 0 on error */
    }

    memcpy(h->data + h->size, buf, len);
    h->size += len;
    return len;
}


/* Read */

int gzread(gzFile file, voidp buf, unsigned len) {
    static original_gzread_t original_gzread = NULL;
    if (!original_gzread) original_gzread = dlsym(RTLD_NEXT, "gzread");

    int got;
    int slot = find_handle(file);
    if (slot < 0){
        got = original_gzread(file, buf, len);
    } else if (handles[slot]->writing){
        got = -1;
    } else{
        // gzread returns an int, so serve at most INT_MAX per call
        // pos can be past the end after a gzseek
        zstd_handle_t *h = handles[slot];
        size_t n = h->pos < h->size ? h->size - h->pos : 0;
        if (n > len) n = len;
        if (n > INT_MAX) n = INT_MAX;
        memcpy(buf, h->data + h->pos, n);
        h->pos += n;
        if (n < len) h->eof = 1;
        got = n;
    }

    if (debug_enabled()){
        fprintf(stderr, "gzread: %p, asked %u, got %d\n", (void*)file, len, got);
    }
    return got;
}

// Like zlib, -1 at the end of the data
static int getc_handle(gzFile file, original_gzgetc_t original){
    int slot = find_handle(file);
    if (slot < 0){
        return original(file);
    }
    zstd_handle_t *h = handles[slot];
    if (h->writing){
        return -1;
    }
    if (h->pos >= h->size){
        h->eof = 1;
        return -1;
    }
    return h->data[h->pos++];
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


/* Seek and state */

// Same rules as zlib: no SEEK_END, reads can move anywhere at or after the
// start, writes only forward (the gap is filled with zeros). Returns the new
// offset in the uncompressed data, or -1
static z_off64_t seek_handle(zstd_handle_t *h, z_off64_t offset, int whence){
    z_off64_t current = h->writing ? (z_off64_t)h->size : (z_off64_t)h->pos;
    if (whence == SEEK_CUR){
        offset += current;
    } else if (whence != SEEK_SET){
        return -1;
    }
    if (offset < 0){
        return -1;
    }

    if (h->writing){
        if (offset < current){
            return -1;
        }
        size_t gap = offset - current;
        if (grow(h, gap) < 0){
            return -1;
        }
        memset(h->data + h->size, 0, gap);
        h->size += gap;
    } else{
        h->pos = offset;
        h->eof = 0;
    }
    return offset;
}

z_off_t gzseek(gzFile file, z_off_t offset, int whence) {
    static original_gzseek_t original_gzseek = NULL;
    if (!original_gzseek) original_gzseek = dlsym(RTLD_NEXT, "gzseek");

    int slot = find_handle(file);
    z_off_t ret = slot < 0 ? original_gzseek(file, offset, whence) : (z_off_t)seek_handle(handles[slot], offset, whence);

    if (debug_enabled()){
        fprintf(stderr, "gzseek: %p, %lld whence %d -> %lld\n", (void*)file, (long long)offset, whence, (long long)ret);
    }
    return ret;
}

z_off64_t gzseek64(gzFile file, z_off64_t offset, int whence) {
    static original_gzseek64_t original_gzseek64 = NULL;
    if (!original_gzseek64) original_gzseek64 = dlsym(RTLD_NEXT, "gzseek64");

    int slot = find_handle(file);
    z_off64_t ret = slot < 0 ? original_gzseek64(file, offset, whence) : seek_handle(handles[slot], offset, whence);

    if (debug_enabled()){
        fprintf(stderr, "gzseek64: %p, %lld whence %d -> %lld\n", (void*)file, (long long)offset, whence, (long long)ret);
    }
    return ret;
}

z_off_t gztell(gzFile file) {
    static original_gztell_t original_gztell = NULL;
    if (!original_gztell) original_gztell = dlsym(RTLD_NEXT, "gztell");

    int slot = find_handle(file);
    if (slot < 0){
        return original_gztell(file);
    }
    zstd_handle_t *h = handles[slot];
    return h->writing ? (z_off_t)h->size : (z_off_t)h->pos;
}

z_off64_t gztell64(gzFile file) {
    static original_gztell64_t original_gztell64 = NULL;
    if (!original_gztell64) original_gztell64 = dlsym(RTLD_NEXT, "gztell64");

    int slot = find_handle(file);
    if (slot < 0){
        return original_gztell64(file);
    }
    zstd_handle_t *h = handles[slot];
    return h->writing ? (z_off64_t)h->size : (z_off64_t)h->pos;
}

// Like zlib, set only once a read has asked for more than was left
int gzeof(gzFile file) {
    static original_gzeof_t original_gzeof = NULL;
    if (!original_gzeof) original_gzeof = dlsym(RTLD_NEXT, "gzeof");

    int slot = find_handle(file);
    return slot < 0 ? original_gzeof(file) : handles[slot]->eof;
}

// Errors on the library's handles are reported by the call that fails, so
// there's never one pending here
const char *gzerror(gzFile file, int *errnum) {
    static original_gzerror_t original_gzerror = NULL;
    if (!original_gzerror) original_gzerror = dlsym(RTLD_NEXT, "gzerror");

    if (find_handle(file) < 0){
        return original_gzerror(file, errnum);
    }
    if (errnum){
        *errnum = Z_OK;
    }
    return "";
}


/* Close */

static int close_write(zstd_handle_t *h){
    int ret = Z_OK;
    double start = now();
    size_t bound = ZSTD_compressBound(h->size);
    void *out = malloc(bound);
    size_t out_size = out ? ZSTD_compress(out, bound, h->data, h->size, h->level) : 0;

    if (!out || ZSTD_isError(out_size)){
        fprintf(stderr, "zstd_lib: compress failed: %s\n", out ? ZSTD_getErrorName(out_size) : "out of memory");
        ret = Z_MEM_ERROR;
    } else if (write_all(h->fd, out, out_size) < 0){
        perror("zstd_lib: write");
        ret = Z_ERRNO;
    } else{
        // One summary line per file written, always printed
        fprintf(stderr, "zstd_lib: level %d, %zu -> %zu bytes, %.2f s\n", h->level, h->size, out_size, now() - start);
    }

    free(out);
    if (close(h->fd) < 0 && ret == Z_OK){
        ret = Z_ERRNO;
    }
    return ret;
}

int gzclose(gzFile file) {
    static original_gzclose_t original_gzclose = NULL;
    if (!original_gzclose) original_gzclose = dlsym(RTLD_NEXT, "gzclose");

    if (debug_enabled()){
        fprintf(stderr, "gzclose: %p\n", (void*)file);
    }

    int slot = find_handle(file);
    if (slot < 0){
        return original_gzclose(file);
    }

    zstd_handle_t *h = handles[slot];
    handles[slot] = NULL;
    int ret = h->writing ? close_write(h) : Z_OK;
    free(h->data);
    free(h);
    return ret;
}
