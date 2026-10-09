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
#include <limits.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define MAX_HANDLES 64
#define DEFAULT_LEVEL 6
// Reads below this offset (headers) stay with zlib; going past it loads the file
#define READ_LAZY_LIMIT (1 << 20)


typedef gzFile (*original_gzopen_t)(const char *path, const char *mode);
typedef gzFile (*original_gzdopen_t)(int fd, const char *mode);
typedef int (*original_gzclose_t)(gzFile file);
typedef int (*original_gzwrite_t)(gzFile file, const void *buf, unsigned int len);
typedef int (*original_deflateInit2__t)(z_streamp strm, int level, int method, int windowBits,
                                         int memLevel, int strategy, const char *version, int stream_size);
typedef int (*original_deflate_t)(z_streamp strm, int flush);
typedef int (*original_deflateEnd_t)(z_streamp strm);
typedef int (*original_deflateSetDictionary_t)(z_streamp strm, const Bytef *dictionary, uInt dictLength);
typedef int (*original_deflateCopy_t)(z_streamp dest, z_streamp source);
typedef uLong (*original_crc32_t)(uLong crc, const Bytef *buf, uInt len);
typedef int (*original_gzread_t)(gzFile file, voidp buf, unsigned len);
typedef z_size_t (*original_gzfread_t)(voidp buf, z_size_t size, z_size_t nitems, gzFile file);
typedef char *(*original_gzgets_t)(gzFile file, char *buf, int len);
typedef int (*original_gzgetc_t)(gzFile file);
typedef int (*original_gzungetc_t)(int c, gzFile file);
typedef z_off64_t (*original_gzseek64_t)(gzFile file, z_off64_t offset, int whence);
typedef z_off64_t (*original_gztell64_t)(gzFile file);
typedef int (*original_gzrewind_t)(gzFile file);
typedef int (*original_gzeof_t)(gzFile file);
typedef void (*original_gzclearerr_t)(gzFile file);


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

// A gz file opened for reading. The caller keeps zlib's own handle (real), so
// any gz function not wrapped here still works on it. Small reads near the
// start (headers) go to zlib; the first access past READ_LAZY_LIMIT
// decompresses the whole file with libdeflate and serves it from memory.
// Once a bulk read reaches the end the copy is freed (drained); going back
// before the end rewinds zlib and starts over
enum { READ_LAZY, READ_LOADED, READ_DRAINED, READ_ZLIB };

typedef struct{
    gzFile real;
    char *path;
    int state;
    unsigned char *data;
    size_t size;
    size_t pos;
    int eof;
} read_handle_t;

static read_handle_t *readers[MAX_HANDLES];

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

// gz reads are only served by libdeflate with COMPRESSION_READS=1
static int reads_enabled(void){
    static int enabled = -1;
    if (enabled < 0){
        const char *env = getenv("COMPRESSION_READS");
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

static int find_reader(gzFile file){
    for (int i = 0; i < MAX_HANDLES; i++){
        if (readers[i] && readers[i]->real == file)
            return i;
    }
    return -1;
}

static void free_reader(int slot){
    read_handle_t *r = readers[slot];
    readers[slot] = NULL;
    free(r->data);
    free(r->path);
    free(r);
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

// Fallback for when libdeflate can't take the level: write the buffered data
// with the real zlib through the open fd (zlib's gzclose closes it)
static int write_with_zlib(deflate_handle_t *h){
    static original_gzdopen_t original_gzdopen = NULL;
    static original_gzwrite_t original_gzwrite = NULL;
    static original_gzclose_t original_gzclose = NULL;
    if (!original_gzdopen) original_gzdopen = zlib_symbol("gzdopen");
    if (!original_gzwrite) original_gzwrite = zlib_symbol("gzwrite");
    if (!original_gzclose) original_gzclose = zlib_symbol("gzclose");

    // zlib levels stop at 9
    int level = h->level < 0 ? DEFAULT_LEVEL : h->level > 9 ? 9 : h->level;
    char mode[8];
    snprintf(mode, sizeof(mode), "wb%d", level);

    gzFile real = original_gzdopen ? original_gzdopen(h->fd, mode) : NULL;
    if (!real){
        close(h->fd);
        return Z_ERRNO;
    }
    size_t pos = 0;
    while (pos < h->size){
        size_t chunk = h->size - pos < (1u << 30) ? h->size - pos : (1u << 30);
        if (original_gzwrite(real, h->data + pos, chunk) <= 0){
            original_gzclose(real);
            return Z_ERRNO;
        }
        pos += chunk;
    }
    return original_gzclose(real);
}

// Reading: open with zlib as usual and remember the handle
static gzFile open_gz_read(const char *path, const char *mode, original_gzopen_t original_gzopen) {
    gzFile real = original_gzopen(path, mode);
    if (!real || !reads_enabled()){
        return real;
    }

    int slot = -1;
    for (int i = 0; i < MAX_HANDLES; i++){
        if (!readers[i]){
            slot = i;
            break;
        }
    }
    if (slot < 0){
        return real;
    }

    read_handle_t *r = calloc(1, sizeof(read_handle_t));
    if (!r){
        return real;
    }
    r->real = real;
    r->path = strdup(path);
    if (!r->path){
        free(r);
        return real;
    }
    readers[slot] = r;

    if (debug_enabled()){
        fprintf(stderr, "gzopen read: path %s, mode %s, file %p\n", path, mode, (void*)real);
    }
    return real;
}

// Shared by gzopen and gzopen64 (programs built with large file support
// call the latter); only the real function differs
static gzFile open_gz(const char *path, const char *mode, original_gzopen_t original_gzopen) {
    if (strchr(mode, 'a')){
        return original_gzopen(path, mode);
    }
    if (!strchr(mode, 'w')){
        return open_gz_read(path, mode, original_gzopen);
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

gzFile gzopen(const char *path, const char *mode) {
    static original_gzopen_t original_gzopen = NULL;
    if (!original_gzopen) original_gzopen = zlib_symbol("gzopen");

    return open_gz(path, mode, original_gzopen);
}

gzFile gzopen64(const char *path, const char *mode) {
    static original_gzopen_t original_gzopen64 = NULL;
    if (!original_gzopen64) original_gzopen64 = zlib_symbol("gzopen64");

    return open_gz(path, mode, original_gzopen64);
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

    int reader = find_reader(file);
    if (reader >= 0){
        free_reader(reader);
        return original_gzclose(file);
    }

    int slot = find_handle(file);
    if (slot < 0){
        return original_gzclose(file);
    }

    deflate_handle_t *h = handles[slot];
    handles[slot] = NULL;
    int ret = Z_OK;
    double start = now();

    struct libdeflate_compressor *compressor = libdeflate_alloc_compressor(h->level);
    if (!compressor){
        // Not "compression_lib:", which the run summary counts as a file
        ret = write_with_zlib(h);
        fprintf(stderr, "compression_lib error: libdeflate can't compress at level %d, wrote %zu bytes with zlib, %.2f s\n",
            h->level, h->size, now() - start);
        free(h->data);
        free(h);
        return ret;
    }
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

/* gz reads (FSL, MRtrix) */

// Uncompressed position zlib has reached on a handle
static z_off64_t zlib_tell(gzFile file){
    static original_gztell64_t original_gztell64 = NULL;
    if (!original_gztell64) original_gztell64 = zlib_symbol("gztell64");
    if (!original_gztell64) original_gztell64 = zlib_symbol("gztell");

    return original_gztell64(file);
}

static int zlib_rewind(gzFile file){
    static original_gzrewind_t original_gzrewind = NULL;
    if (!original_gzrewind) original_gzrewind = zlib_symbol("gzrewind");

    return original_gzrewind(file);
}

static z_off64_t zlib_seek(gzFile file, z_off64_t offset){
    static original_gzseek64_t original_gzseek64 = NULL;
    if (!original_gzseek64) original_gzseek64 = zlib_symbol("gzseek64");
    if (!original_gzseek64) original_gzseek64 = zlib_symbol("gzseek");

    return original_gzseek64(file, offset, SEEK_SET);
}

// Decompress the whole file with libdeflate and serve it from memory from
// zlib's current position on. Files that aren't a single gzip member
// (plain, multi-member, corrupt) are left to zlib: returns -1
static int materialise(read_handle_t *r){
    z_off64_t pos = zlib_tell(r->real);
    r->state = READ_ZLIB;
    if (pos < 0){
        return -1;
    }

    int fd = open(r->path, O_RDONLY);
    if (fd < 0){
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < 18){
        close(fd);
        return -1;
    }
    size_t in_size = st.st_size;
    unsigned char *in = mmap(NULL, in_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (in == MAP_FAILED){
        return -1;
    }
    madvise(in, in_size, MADV_SEQUENTIAL);

    double start = now();
    unsigned char *out = NULL;
    size_t out_size = 0;
    int tries = 0;
    int ok = 0;

    if (in[0] == 0x1f && in[1] == 0x8b && in[2] == 8){
        // The trailer only holds the size mod 2^32. Deflate barely expands
        // data, so start from the first candidate near the compressed size
        // and add 4 GiB while libdeflate runs out of space
        size_t n = in[in_size - 4] | (size_t)in[in_size - 3] << 8
            | (size_t)in[in_size - 2] << 16 | (size_t)in[in_size - 1] << 24;
        size_t lower = in_size > 1024 ? in_size - in_size / 512 - 1024 : 0;
        while (n < lower){
            n += (size_t)1 << 32;
        }

        struct libdeflate_decompressor *d = libdeflate_alloc_decompressor();
        while (d && tries < 16){
            tries++;
            free(out);
            out = malloc(n ? n : 1);
            if (!out){
                break;
            }
            size_t in_used = 0;
            enum libdeflate_result res = libdeflate_gzip_decompress_ex(d, in, in_size, out, n, &in_used, &out_size);
            if (res == LIBDEFLATE_INSUFFICIENT_SPACE){
                n += (size_t)1 << 32;
                continue;
            }
            // More data after the first member (concatenated gzip) is left to zlib
            ok = res == LIBDEFLATE_SUCCESS && in_used == in_size;
            break;
        }
        libdeflate_free_decompressor(d);
    }
    munmap(in, in_size);

    if (!ok){
        free(out);
        if (debug_enabled()){
            fprintf(stderr, "gz read: leaving %s to zlib\n", r->path);
        }
        return -1;
    }

    // One summary line per loaded file, always printed
    fprintf(stderr, "compression_lib: read %s, %zu -> %zu bytes, %d tries, %.2f s\n",
        r->path, in_size, out_size, tries, now() - start);

    r->data = out;
    r->size = out_size;
    r->pos = pos;
    r->eof = 0;
    r->state = READ_LOADED;
    // zlib's gzgetc() macro reads straight from zlib's buffer while have > 0;
    // emptying it sends the macro to the wrapped gzgetc
    r->real->have = 0;
    return 0;
}

// A drained file needs its data again: rewind zlib, whose state is stale since
// loading, and continue lazily from the same position
static void reload(read_handle_t *r){
    size_t target = r->pos;
    r->state = READ_ZLIB;
    if (zlib_rewind(r->real) < 0){
        return;
    }
    r->state = READ_LAZY;
    if (target <= READ_LAZY_LIMIT){
        if (zlib_seek(r->real, target) < 0){
            r->state = READ_ZLIB;
        }
        return;
    }
    if (materialise(r) == 0){
        r->pos = target;
    }
}

// The reader to serve an access of len bytes from memory, or NULL to let zlib
// handle it. A lazy file is loaded once an access goes past READ_LAZY_LIMIT
static read_handle_t *memory_reader(gzFile file, size_t len){
    int slot = find_reader(file);
    if (slot < 0){
        return NULL;
    }
    read_handle_t *r = readers[slot];
    if (r->state == READ_DRAINED && r->pos < r->size){
        reload(r);
    }
    if (r->state == READ_LAZY){
        z_off64_t pos = zlib_tell(file);
        if (pos >= 0 && (size_t)pos + len > READ_LAZY_LIMIT){
            materialise(r);
        }
    }
    return r->state == READ_LOADED || r->state == READ_DRAINED ? r : NULL;
}

// A reader served from memory (loaded or drained), or NULL
static read_handle_t *loaded_reader(gzFile file){
    int slot = find_reader(file);
    if (slot < 0){
        return NULL;
    }
    int state = readers[slot]->state;
    return state == READ_LOADED || state == READ_DRAINED ? readers[slot] : NULL;
}

static size_t read_memory(read_handle_t *r, void *buf, size_t len){
    size_t available = r->pos < r->size ? r->size - r->pos : 0;
    size_t n = len < available ? len : available;
    if (n > 0){
        memcpy(buf, r->data + r->pos, n);
        r->pos += n;
    }
    // Like zlib, eof is only set once a read asks for more than is left
    if (n < len){
        r->eof = 1;
    }
    // Tools read the image once into their own memory: drop our copy
    if (r->state == READ_LOADED && r->pos >= r->size){
        free(r->data);
        r->data = NULL;
        r->state = READ_DRAINED;
    }
    return n;
}

int gzread(gzFile file, voidp buf, unsigned len) {
    static original_gzread_t original_gzread = NULL;
    if (!original_gzread) original_gzread = zlib_symbol("gzread");

    if (debug_enabled()){
        fprintf(stderr, "gzread: file %p, len %u\n", (void*)file, len);
    }

    read_handle_t *r = memory_reader(file, len);
    if (!r){
        return original_gzread(file, buf, len);
    }
    if (len > INT_MAX){
        return -1;
    }
    return (int)read_memory(r, buf, len);
}


z_size_t gzfread(voidp buf, z_size_t size, z_size_t nitems, gzFile file) {
    static original_gzfread_t original_gzfread = NULL;
    if (!original_gzfread) original_gzfread = zlib_symbol("gzfread");

    if (debug_enabled()){
        fprintf(stderr, "gzfread: file %p, size %zu, nitems %zu\n", (void*)file, (size_t)size, (size_t)nitems);
    }

    if (size == 0 || nitems > SIZE_MAX / size){
        return original_gzfread(buf, size, nitems, file);
    }
    read_handle_t *r = memory_reader(file, size * nitems);
    if (!r){
        return original_gzfread(buf, size, nitems, file);
    }
    return read_memory(r, buf, size * nitems) / size;
}


char *gzgets(gzFile file, char *buf, int len) {
    static original_gzgets_t original_gzgets = NULL;
    if (!original_gzgets) original_gzgets = zlib_symbol("gzgets");

    read_handle_t *r = len > 0 ? memory_reader(file, len) : NULL;
    if (!r || !buf){
        return original_gzgets(file, buf, len);
    }

    // Up to len - 1 bytes, stopping after a newline
    size_t n = 0;
    while (n < (size_t)len - 1 && r->pos < r->size){
        char c = r->data[r->pos++];
        buf[n++] = c;
        if (c == '\n'){
            break;
        }
    }
    buf[n] = '\0';
    if (r->pos >= r->size && (n == 0 || buf[n - 1] != '\n')){
        r->eof = 1;
    }
    return n > 0 ? buf : NULL;
}


static int getc_gz(gzFile file, original_gzgetc_t original_getc) {
    read_handle_t *r = memory_reader(file, 1);
    if (!r){
        return original_getc(file);
    }
    if (r->pos < r->size){
        return r->data[r->pos++];
    }
    r->eof = 1;
    return -1;
}

// Parentheses keep zlib.h's gzgetc() macro from expanding here
int (gzgetc)(gzFile file) {
    static original_gzgetc_t original_gzgetc = NULL;
    if (!original_gzgetc) original_gzgetc = zlib_symbol("gzgetc");

    return getc_gz(file, original_gzgetc);
}

int gzgetc_(gzFile file) {
    static original_gzgetc_t original_gzgetc_ = NULL;
    if (!original_gzgetc_) original_gzgetc_ = zlib_symbol("gzgetc_");

    return getc_gz(file, original_gzgetc_);
}


int gzungetc(int c, gzFile file) {
    static original_gzungetc_t original_gzungetc = NULL;
    if (!original_gzungetc) original_gzungetc = zlib_symbol("gzungetc");

    read_handle_t *r = loaded_reader(file);
    if (!r){
        return original_gzungetc(c, file);
    }
    // The buffer is our own copy, so push back by overwriting the byte before
    // pos; not supported once drained
    if (c < 0 || !r->data || r->pos == 0 || r->pos > r->size){
        return -1;
    }
    r->data[--r->pos] = (unsigned char)c;
    r->eof = 0;
    return c;
}


// Shared by gzseek and gzseek64; z_off_t and z_off64_t are both 64 bits on x86_64
static z_off64_t seek_gz(gzFile file, z_off64_t offset, int whence, original_gzseek64_t original_seek) {
    if (debug_enabled()){
        fprintf(stderr, "gzseek: file %p, offset %lld, whence %d\n", (void*)file, (long long)offset, whence);
    }

    int slot = find_reader(file);
    // zlib refuses anything but SEEK_SET and SEEK_CUR without touching its state
    if (slot < 0 || readers[slot]->state == READ_ZLIB || (whence != SEEK_SET && whence != SEEK_CUR)){
        return original_seek(file, offset, whence);
    }

    read_handle_t *r = readers[slot];
    z_off64_t current = r->state == READ_LAZY ? zlib_tell(file) : (z_off64_t)r->pos;
    z_off64_t target = whence == SEEK_SET ? offset : current + offset;
    if (r->state == READ_LAZY && target > READ_LAZY_LIMIT){
        materialise(r);
    }
    if (r->state != READ_LOADED && r->state != READ_DRAINED){
        return original_seek(file, offset, whence);
    }
    if (target < 0){
        return -1;
    }
    r->pos = target;
    r->eof = 0;
    return target;
}

z_off_t gzseek(gzFile file, z_off_t offset, int whence) {
    static original_gzseek64_t original_gzseek = NULL;
    if (!original_gzseek) original_gzseek = zlib_symbol("gzseek");

    return seek_gz(file, offset, whence, original_gzseek);
}

z_off64_t gzseek64(gzFile file, z_off64_t offset, int whence) {
    static original_gzseek64_t original_gzseek64 = NULL;
    if (!original_gzseek64) original_gzseek64 = zlib_symbol("gzseek64");

    return seek_gz(file, offset, whence, original_gzseek64);
}


static z_off64_t tell_gz(gzFile file, original_gztell64_t original_tell) {
    read_handle_t *r = loaded_reader(file);
    return r ? (z_off64_t)r->pos : original_tell(file);
}

z_off_t gztell(gzFile file) {
    static original_gztell64_t original_gztell = NULL;
    if (!original_gztell) original_gztell = zlib_symbol("gztell");

    return tell_gz(file, original_gztell);
}

z_off64_t gztell64(gzFile file) {
    static original_gztell64_t original_gztell64 = NULL;
    if (!original_gztell64) original_gztell64 = zlib_symbol("gztell64");

    return tell_gz(file, original_gztell64);
}


int gzrewind(gzFile file) {
    static original_gzrewind_t original_gzrewind = NULL;
    if (!original_gzrewind) original_gzrewind = zlib_symbol("gzrewind");

    read_handle_t *r = loaded_reader(file);
    if (!r){
        return original_gzrewind(file);
    }
    r->pos = 0;
    r->eof = 0;
    return 0;
}


int gzeof(gzFile file) {
    static original_gzeof_t original_gzeof = NULL;
    if (!original_gzeof) original_gzeof = zlib_symbol("gzeof");

    read_handle_t *r = loaded_reader(file);
    return r ? r->eof : original_gzeof(file);
}


void gzclearerr(gzFile file) {
    static original_gzclearerr_t original_gzclearerr = NULL;
    if (!original_gzclearerr) original_gzclearerr = zlib_symbol("gzclearerr");

    read_handle_t *r = loaded_reader(file);
    if (r){
        r->eof = 0;
    }
    original_gzclearerr(file);
}


int gzclose_r(gzFile file) {
    static original_gzclose_t original_gzclose_r = NULL;
    if (!original_gzclose_r) original_gzclose_r = zlib_symbol("gzclose_r");

    int reader = find_reader(file);
    if (reader >= 0){
        free_reader(reader);
    }
    return original_gzclose_r(file);
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
        // The real stream is already set up, so zlib does this one
        fprintf(stderr, "compression_lib error: libdeflate can't compress at level %d, stream left to zlib\n", level);
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
    if (slot < 0){
        return original_deflateEnd(strm);
    }

    stream_handle_t *s = streams[slot];
    // One summary line per stream, always printed
    fprintf(stderr, "compression_lib: stream level %d, %lu calls, %lu sync flushes, %lu -> %lu bytes, %.2f s\n",
        s->level, s->calls, s->sync_flushes, strm->total_in, strm->total_out, s->compress_time);
    int incomplete = s->calls > 0 && !(s->finished && s->out_pos == s->out_size);
    free_stream(slot);

    // The real stream never saw deflate(), and zlib 1.2.11 starts raw streams
    // busy, so its deflateEnd reports Z_DATA_ERROR; answer from our own state
    int ret = original_deflateEnd(strm);
    if (ret == Z_STREAM_ERROR){
        return ret;
    }
    return incomplete ? Z_DATA_ERROR : Z_OK;
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


// Python's gzip checksums every chunk it writes or reads; libdeflate's crc32
// gives the same values, faster
uLong crc32(uLong crc, const Bytef *buf, uInt len) {
    static original_crc32_t original_crc32 = NULL;
    if (!original_crc32) original_crc32 = zlib_symbol("crc32");

    if (!streams_enabled()){
        return original_crc32(crc, buf, len);
    }
    // zlib returns the initial value for a NULL buffer, whatever crc is
    if (!buf){
        return 0;
    }
    return libdeflate_crc32(crc, buf, len);
}
