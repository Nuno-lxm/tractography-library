#define _GNU_SOURCE

#include <stdio.h>
#include <dlfcn.h>
#include <zlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <libdeflate.h>
#include <stdlib.h>
#include <time.h>

#define MAX_HANDLES 64
#define DEFAULT_LEVEL 6


typedef gzFile (*original_gzopen_t)(const char *path, const char *mode);
typedef int (*original_gzclose_t)(gzFile file);
typedef int (*original_gzwrite_t)(gzFile file, const void *buf, unsigned int len);
typedef int (*original_deflateInit2__t)(z_streamp strm, int level, int method, int windowBits,
                                         int memLevel, int strategy, const char *version, int stream_size);
typedef int (*original_deflate_t)(z_streamp strm, int flush);
typedef int (*original_deflateEnd_t)(z_streamp strm);


typedef struct{
    int fd;
    int level;
    unsigned char *data;
    size_t size;
    size_t capacity;
} deflate_handle_t;

static deflate_handle_t *handles[MAX_HANDLES];

// Per-call prints only with COMPRESSION_DEBUG set; they cost a write to stderr each
static int debug_enabled(void){
    static int enabled = -1;
    if (enabled < 0){
        enabled = getenv("COMPRESSION_DEBUG") != NULL;
    }
    return enabled;
}

// Python loads its zlib module (and libz) with RTLD_LOCAL, so RTLD_NEXT can't
// see libz there; fall back to the copy of libz.so.1 that is already loaded
static void *zlib_symbol(const char *name){
    void *sym = dlsym(RTLD_NEXT, name);
    if (!sym){
        void *libz = dlopen("libz.so.1", RTLD_LAZY | RTLD_NOLOAD);
        if (!libz){
            libz = dlopen("libz.so.1", RTLD_LAZY);
        }
        if (libz){
            sym = dlsym(libz, name);
        }
    }
    return sym;
}

static double now(void){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int find_handle(gzFile file){
    for (int i= 0; i < MAX_HANDLES; i++){
        if (handles[i] && (gzFile)handles[i] == file)
            return i;
    }
    return -1;
}

static int parse_level(const char *mode){
    const char *env = getenv("COMPRESSION_LEVEL");
    if (env && *env){
        return atoi(env);
    }
    for (; *mode; mode++){
        if (*mode >= '0' && *mode <= '9'){
            return *mode - '0';
        }
    }
    return DEFAULT_LEVEL;
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

gzFile gzopen(const char *path, const char *mode) {
    static original_gzopen_t original_gzopen = NULL;
    if (!original_gzopen) original_gzopen = dlsym(RTLD_NEXT, "gzopen");

    if (!strchr(mode, 'w')){
        return original_gzopen(path, mode);
    }

    int slot = -1;
    for (int i = 0; i < MAX_HANDLES; i++) {
        if (!handles[i]) {
            slot = i;
            break;
        }
    }
    if (slot < 0){
        return original_gzopen(path, mode);
    }

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0){
        perror("open");
        return NULL;
    }

    deflate_handle_t *h = calloc(1, sizeof(deflate_handle_t));
    h->fd = fd;
    h->level= parse_level(mode);
    handles[slot] = h;
    
    if (debug_enabled()){
        fprintf(stderr, "gzopen called for path: %s, mode: %s\n", path, mode);
    }
    return (gzFile)h;
}


int gzwrite(gzFile file, const void *buf, unsigned int len) {
    static original_gzwrite_t original_gzwrite = NULL;
    if (!original_gzwrite) original_gzwrite = dlsym(RTLD_NEXT, "gzwrite");

    int slot = find_handle(file);
    if (slot < 0){
        return original_gzwrite(file, buf, len);
    }

    deflate_handle_t *h = handles[slot];
    if (h->size + len > h->capacity){
        size_t new_capacity = h->capacity ? h->capacity : 65536;
        while (new_capacity < h->size + len){
            new_capacity *= 2;
        }
        unsigned char *new_data = realloc(h->data, new_capacity);
        if (!new_data){
            perror("realloc");
            return 0; /*gzwrite returns 0 on error*/
        }
        h->data = new_data;
        h->capacity = new_capacity;
    }

    memcpy(h->data + h->size, buf, len);
    h->size += len;

    if (debug_enabled()){
        fprintf(stderr, "gzwrite called for file: %p, buffer: %p, length: %u\n", (void*)file, buf, len);
    }
    return len;
}


int gzclose(gzFile file) {
    static original_gzclose_t original_gzclose = NULL;
    if (!original_gzclose) original_gzclose = dlsym(RTLD_NEXT, "gzclose");

    int slot = find_handle(file);
    if (slot < 0){
        return original_gzclose(file);
    }

    deflate_handle_t *h = handles[slot];
    handles[slot] = NULL;
    int ret = Z_OK;
    double start = now();

    struct libdeflate_compressor *compressor = libdeflate_alloc_compressor(h->level);
    size_t bound = libdeflate_gzip_compress_bound(compressor, h->size);
    void *out = malloc(bound);
    size_t out_size = libdeflate_gzip_compress(compressor, h->data, h->size, out, bound);

    if (out_size == 0 || write_all(h->fd, out, out_size) < 0){
        perror("write_all");
        ret = Z_ERRNO;
    }

    // One summary line per file, always printed
    fprintf(stderr, "compression_lib: level %d, %zu -> %zu bytes, %.2f s\n",
        h->level, h->size, out_size, now() - start);

    libdeflate_free_compressor(compressor);
    free(out);
    free(h->data);
    close(h->fd);
    free(h);

    if (debug_enabled()){
        fprintf(stderr, "gzclose called for file: %p\n", (void*)file);
    }
    return ret;
}

/* Raw deflate streams (Python's gzip/zlib modules) */

int deflateInit2_(z_streamp strm, int level, int method, int windowBits,
                  int memLevel, int strategy, const char *version, int stream_size) {
    static original_deflateInit2__t original_deflateInit2_ = NULL;
    if (!original_deflateInit2_) original_deflateInit2_ = zlib_symbol("deflateInit2_");

    if (debug_enabled()){
        fprintf(stderr, "deflateInit2_: strm %p, level %d, windowBits %d, memLevel %d, strategy %d\n",
            (void*)strm, level, windowBits, memLevel, strategy);
    }
    return original_deflateInit2_(strm, level, method, windowBits, memLevel, strategy, version, stream_size);
}


int deflate(z_streamp strm, int flush) {
    static original_deflate_t original_deflate = NULL;
    if (!original_deflate) original_deflate = zlib_symbol("deflate");

    if (debug_enabled()){
        fprintf(stderr, "deflate: strm %p, flush %d, avail_in %u, avail_out %u\n",
            (void*)strm, flush, strm->avail_in, strm->avail_out);
    }
    return original_deflate(strm, flush);
}


int deflateEnd(z_streamp strm) {
    static original_deflateEnd_t original_deflateEnd = NULL;
    if (!original_deflateEnd) original_deflateEnd = zlib_symbol("deflateEnd");

    if (debug_enabled()){
        fprintf(stderr, "deflateEnd: strm %p, total_in %lu, total_out %lu\n",
            (void*)strm, strm->total_in, strm->total_out);
    }
    return original_deflateEnd(strm);
}
