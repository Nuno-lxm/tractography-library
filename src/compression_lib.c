#define _GNU_SOURCE

#include <stdio.h>
#include <dlfcn.h>
#include <zlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <libdeflate.h>
#include <stdlib.h>

#define MAX_HANDLES 64
#define DEFAULT_LEVEL 6


typedef gzFile (*original_gzopen_t)(const char *path, const char *mode);
typedef int (*original_gzclose_t)(gzFile file);
typedef int (*original_gzwrite_t)(gzFile file, const void *buf, unsigned int len);


typedef struct{
    int fd;
    int level;
    unsigned char *data;
    size_t size;
    size_t capacity;
} deflate_handle_t;

static deflate_handle_t *handles[MAX_HANDLES];

static int find_handle(gzFile file){
    for (int i= 0; i < MAX_HANDLES; i++){
        if (handles[i] && (gzFile)handles[i] == file)
            return i;
    }
    return -1;
}

static int parse_level(const char *mode){
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
    
    fprintf(stderr, "gzopen called for path: %s, mode: %s\n", path, mode);
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

    fprintf(stderr, "gzwrite called for file: %p, buffer: %p, length: %u\n", (void*)file, buf, len);
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

    struct libdeflate_compressor *compressor = libdeflate_alloc_compressor(h->level);
    size_t bound = libdeflate_gzip_compress_bound(compressor, h->size);
    void *out = malloc(bound);
    size_t out_size = libdeflate_gzip_compress(compressor, h->data, h->size, out, bound);

    if (out_size == 0 || write_all(h->fd, out, out_size) < 0){
        perror("write_all");
        ret = Z_ERRNO;
    }

    libdeflate_free_compressor(compressor);
    free(out);
    free(h->data);
    close(h->fd);
    free(h);
    
    fprintf(stderr, "gzclose called for file: %p\n", (void*)file);
    return ret;
}