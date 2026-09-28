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
typedef int (*original_deflateSetDictionary_t)(z_streamp strm, const Bytef *dictionary, uInt dictLength);
typedef int (*original_deflateCopy_t)(z_streamp dest, z_streamp source);


typedef struct{
    int fd;
    int level;
    unsigned char *data;
    size_t size;
    size_t capacity;
} deflate_handle_t;

static deflate_handle_t *handles[MAX_HANDLES];

// A raw deflate stream taken over from zlib: input is buffered until Z_FINISH,
// then compressed in one go and handed out across the Z_FINISH calls
typedef struct{
    z_streamp strm;
    int level;
    struct libdeflate_compressor *compressor;
    unsigned char *in;
    size_t in_size;
    size_t in_capacity;
    unsigned char *out;
    size_t out_size;
    size_t out_pos;
    int finished;
    unsigned long calls;
    unsigned long sync_flushes;
    double compress_time;
} stream_handle_t;

static stream_handle_t *streams[MAX_HANDLES];

// Per-call prints only with COMPRESSION_DEBUG set; they cost a write to stderr each
static int debug_enabled(void){
    static int enabled = -1;
    if (enabled < 0){
        enabled = getenv("COMPRESSION_DEBUG") != NULL;
    }
    return enabled;
}

// Raw deflate streams are only taken over with COMPRESSION_STREAMS=1
static int streams_enabled(void){
    static int enabled = -1;
    if (enabled < 0){
        const char *env = getenv("COMPRESSION_STREAMS");
        enabled = env && atoi(env) != 0;
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

static int find_stream(z_streamp strm){
    for (int i = 0; i < MAX_HANDLES; i++){
        if (streams[i] && streams[i]->strm == strm)
            return i;
    }
    return -1;
}

static void free_stream(int slot){
    stream_handle_t *s = streams[slot];
    streams[slot] = NULL;
    libdeflate_free_compressor(s->compressor);
    free(s->in);
    free(s->out);
    free(s);
}

// COMPRESSION_LEVEL overrides whatever level the caller asked for
static int env_level(int fallback){
    const char *env = getenv("COMPRESSION_LEVEL");
    if (env && *env){
        return atoi(env);
    }
    return fallback;
}

static int parse_level(const char *mode){
    for (; *mode; mode++){
        if (*mode >= '0' && *mode <= '9'){
            return env_level(*mode - '0');
        }
    }
    return env_level(DEFAULT_LEVEL);
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

    // Always init the real stream, so deflateEnd and pass-through keep working
    int ret = original_deflateInit2_(strm, level, method, windowBits, memLevel, strategy, version, stream_size);
    if (ret != Z_OK || !streams_enabled()){
        return ret;
    }

    // Only raw deflate (windowBits -15, what Python's gzip uses); libz's own
    // gz streams use 31 and also reach this wrapper through gzread/gzwrite
    if (windowBits != -MAX_WBITS || method != Z_DEFLATED || strategy != Z_DEFAULT_STRATEGY){
        return ret;
    }

    int slot = -1;
    for (int i = 0; i < MAX_HANDLES; i++){
        if (!streams[i]){
            slot = i;
            break;
        }
    }
    if (slot < 0){
        return ret;
    }

    if (level == Z_DEFAULT_COMPRESSION){
        level = DEFAULT_LEVEL;
    }
    level = env_level(level);

    struct libdeflate_compressor *compressor = libdeflate_alloc_compressor(level);
    if (!compressor){
        return ret;
    }

    stream_handle_t *s = calloc(1, sizeof(stream_handle_t));
    if (!s){
        libdeflate_free_compressor(compressor);
        return ret;
    }
    s->strm = strm;
    s->level = level;
    s->compressor = compressor;
    streams[slot] = s;
    return ret;
}


// Move the caller's input into the stream's buffer
static int buffer_input(stream_handle_t *s){
    z_streamp strm = s->strm;
    size_t len = strm->avail_in;
    if (len == 0){
        return Z_OK;
    }

    if (s->in_size + len > s->in_capacity){
        size_t new_capacity = s->in_capacity ? s->in_capacity : 65536;
        while (new_capacity < s->in_size + len){
            new_capacity *= 2;
        }
        unsigned char *new_in = realloc(s->in, new_capacity);
        if (!new_in){
            perror("realloc");
            return Z_MEM_ERROR;
        }
        s->in = new_in;
        s->in_capacity = new_capacity;
    }

    memcpy(s->in + s->in_size, strm->next_in, len);
    s->in_size += len;
    strm->next_in += len;
    strm->avail_in = 0;
    strm->total_in += len;
    return Z_OK;
}

// Compress everything buffered so far into s->out, once
static int compress_stream(stream_handle_t *s){
    double start = now();
    size_t bound = libdeflate_deflate_compress_bound(s->compressor, s->in_size);
    s->out = malloc(bound);
    if (!s->out){
        perror("malloc");
        return Z_MEM_ERROR;
    }

    s->out_size = libdeflate_deflate_compress(s->compressor, s->in, s->in_size, s->out, bound);
    if (s->out_size == 0){
        return Z_STREAM_ERROR;
    }

    free(s->in);
    s->in = NULL;
    s->in_capacity = 0;
    s->finished = 1;
    s->compress_time = now() - start;
    return Z_OK;
}

int deflate(z_streamp strm, int flush) {
    static original_deflate_t original_deflate = NULL;
    if (!original_deflate) original_deflate = zlib_symbol("deflate");

    if (debug_enabled()){
        fprintf(stderr, "deflate: strm %p, flush %d, avail_in %u, avail_out %u\n",
            (void*)strm, flush, strm->avail_in, strm->avail_out);
    }

    int slot = find_stream(strm);
    if (slot < 0){
        return original_deflate(strm, flush);
    }

    stream_handle_t *s = streams[slot];
    s->calls++;

    if (s->finished){
        // zlib only accepts more Z_FINISH calls, without input, once finishing
        if (flush != Z_FINISH || strm->avail_in > 0){
            return Z_STREAM_ERROR;
        }
    } else {
        int ret = buffer_input(s);
        if (ret != Z_OK){
            return ret;
        }
        if (flush != Z_FINISH){
            // libdeflate can't flush mid-stream: the bytes come out at Z_FINISH,
            // which still gives a valid file
            if (flush != Z_NO_FLUSH){
                s->sync_flushes++;
            }
            return Z_OK;
        }
        ret = compress_stream(s);
        if (ret != Z_OK){
            return ret;
        }
    }

    // Hand out as much compressed output as fits; the caller keeps calling
    // with Z_FINISH and a fresh buffer until Z_STREAM_END
    size_t remaining = s->out_size - s->out_pos;
    if (remaining > 0 && strm->avail_out == 0){
        return Z_BUF_ERROR;
    }
    size_t n = remaining < strm->avail_out ? remaining : strm->avail_out;
    memcpy(strm->next_out, s->out + s->out_pos, n);
    s->out_pos += n;
    strm->next_out += n;
    strm->avail_out -= n;
    strm->total_out += n;

    return s->out_pos == s->out_size ? Z_STREAM_END : Z_OK;
}


int deflateEnd(z_streamp strm) {
    static original_deflateEnd_t original_deflateEnd = NULL;
    if (!original_deflateEnd) original_deflateEnd = zlib_symbol("deflateEnd");

    if (debug_enabled()){
        fprintf(stderr, "deflateEnd: strm %p, total_in %lu, total_out %lu\n",
            (void*)strm, strm->total_in, strm->total_out);
    }

    int slot = find_stream(strm);
    if (slot >= 0){
        stream_handle_t *s = streams[slot];
        // One summary line per stream, always printed
        fprintf(stderr, "compression_lib: stream level %d, %lu calls, %lu sync flushes, %lu -> %lu bytes, %.2f s\n",
            s->level, s->calls, s->sync_flushes, strm->total_in, strm->total_out, s->compress_time);
        free_stream(slot);
    }
    return original_deflateEnd(strm);
}


int deflateSetDictionary(z_streamp strm, const Bytef *dictionary, uInt dictLength) {
    static original_deflateSetDictionary_t original_deflateSetDictionary = NULL;
    if (!original_deflateSetDictionary) original_deflateSetDictionary = zlib_symbol("deflateSetDictionary");

    // libdeflate has no preset dictionaries: give the stream back to zlib.
    // It comes right after init, so nothing has been buffered yet
    int slot = find_stream(strm);
    if (slot >= 0 && streams[slot]->in_size == 0 && !streams[slot]->finished){
        free_stream(slot);
    } else if (slot >= 0){
        return Z_STREAM_ERROR;
    }
    return original_deflateSetDictionary(strm, dictionary, dictLength);
}


int deflateCopy(z_streamp dest, z_streamp source) {
    static original_deflateCopy_t original_deflateCopy = NULL;
    if (!original_deflateCopy) original_deflateCopy = zlib_symbol("deflateCopy");

    // The buffered input lives in our handle, not in zlib's state, so a copy
    // made by zlib would silently lose it
    if (find_stream(source) >= 0){
        return Z_STREAM_ERROR;
    }
    return original_deflateCopy(dest, source);
}
