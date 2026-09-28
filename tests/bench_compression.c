/*
Compare zlib, libdeflate and zstd on real images: compression ratio,
compression and decompression speed, whole buffer in memory.

Usage: bench_compression [-r repeats] [-t threads] FILE...
FILE should be uncompressed (.nii). Each row is the best of the repeats.
*/

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <zlib.h>
#include <libdeflate.h>
#include <zstd.h>

typedef struct {
    const char *codec;
    int level;
    int threads;
} row_t;

static double now(void){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static unsigned char *read_file(const char *path, size_t *size){
    int fd = open(path, O_RDONLY);
    if (fd < 0){
        perror(path);
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) < 0){
        perror("fstat");
        close(fd);
        return NULL;
    }

    unsigned char *buf = malloc(st.st_size ? st.st_size : 1);
    if (!buf){
        perror("malloc");
        close(fd);
        return NULL;
    }

    size_t done = 0;
    while (done < (size_t)st.st_size){
        ssize_t n = read(fd, buf + done, st.st_size - done);
        if (n <= 0){
            perror("read");
            free(buf);
            close(fd);
            return NULL;
        }
        done += n;
    }

    close(fd);
    *size = done;
    return buf;
}

/* Codecs: each returns the output size, or 0 on error */

static size_t bound(const row_t *row, size_t size){
    if (strcmp(row->codec, "zlib") == 0){
        return compressBound(size);
    }
    if (strcmp(row->codec, "libdeflate") == 0){
        // NULL compressor gives a bound valid for any level
        return libdeflate_gzip_compress_bound(NULL, size);
    }
    return ZSTD_compressBound(size);
}

static size_t compress_row(const row_t *row, const unsigned char *in, size_t size,
                           unsigned char *out, size_t capacity){
    if (strcmp(row->codec, "zlib") == 0){
        uLongf out_size = capacity;
        if (compress2(out, &out_size, in, size, row->level) != Z_OK){
            return 0;
        }
        return out_size;
    }

    if (strcmp(row->codec, "libdeflate") == 0){
        struct libdeflate_compressor *c = libdeflate_alloc_compressor(row->level);
        if (!c){
            return 0;
        }
        size_t out_size = libdeflate_gzip_compress(c, in, size, out, capacity);
        libdeflate_free_compressor(c);
        return out_size;
    }

    if (row->threads <= 1){
        size_t out_size = ZSTD_compress(out, capacity, in, size, row->level);
        return ZSTD_isError(out_size) ? 0 : out_size;
    }

    ZSTD_CCtx *cctx = ZSTD_createCCtx();
    if (!cctx){
        return 0;
    }
    size_t out_size = 0;
    if (!ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, row->level)) &&
        !ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_nbWorkers, row->threads))){
        out_size = ZSTD_compress2(cctx, out, capacity, in, size);
        if (ZSTD_isError(out_size)){
            out_size = 0;
        }
    }
    ZSTD_freeCCtx(cctx);
    return out_size;
}

static size_t decompress_row(const row_t *row, const unsigned char *in, size_t size,
                             unsigned char *out, size_t capacity){
    if (strcmp(row->codec, "zlib") == 0){
        uLongf out_size = capacity;
        if (uncompress(out, &out_size, in, size) != Z_OK){
            return 0;
        }
        return out_size;
    }

    if (strcmp(row->codec, "libdeflate") == 0){
        struct libdeflate_decompressor *d = libdeflate_alloc_decompressor();
        if (!d){
            return 0;
        }
        size_t out_size = 0;
        enum libdeflate_result r = libdeflate_gzip_decompress(d, in, size, out, capacity, &out_size);
        libdeflate_free_decompressor(d);
        return r == LIBDEFLATE_SUCCESS ? out_size : 0;
    }

    size_t out_size = ZSTD_decompress(out, capacity, in, size);
    return ZSTD_isError(out_size) ? 0 : out_size;
}

/* zstd multithreading is a build option of libzstd */
static int zstd_has_threads(void){
    ZSTD_bounds b = ZSTD_cParam_getBounds(ZSTD_c_nbWorkers);
    return !ZSTD_isError(b.error) && b.upperBound > 0;
}

static void bench_row(const row_t *row, const unsigned char *in, size_t size,
                      unsigned char *restored, int repeats){
    size_t capacity = bound(row, size);
    unsigned char *out = malloc(capacity);
    if (!out){
        perror("malloc");
        return;
    }
    // Touch the pages first so the timings measure the codec, not page faults
    memset(out, 0, capacity);

    double best_comp = 0, best_decomp = 0;
    size_t out_size = 0;
    int ok = 1;

    for (int r = 0; r < repeats && ok; r++){
        double start = now();
        out_size = compress_row(row, in, size, out, capacity);
        double comp = now() - start;

        start = now();
        size_t restored_size = out_size ? decompress_row(row, out, out_size, restored, size) : 0;
        double decomp = now() - start;

        ok = out_size > 0 && restored_size == size && memcmp(in, restored, size) == 0;
        if (r == 0 || comp < best_comp) best_comp = comp;
        if (r == 0 || decomp < best_decomp) best_decomp = decomp;
    }

    if (!ok){
        printf("%-10s %5d %7d  FAILED round trip\n", row->codec, row->level, row->threads);
    } else {
        printf("%-10s %5d %7d %9.1f %9.1f %6.3f %8.2f %9.1f %8.2f %9.1f\n",
            row->codec, row->level, row->threads,
            size / 1e6, out_size / 1e6, (double)size / out_size,
            best_comp, size / 1e6 / best_comp,
            best_decomp, size / 1e6 / best_decomp);
    }
    fflush(stdout);
    free(out);
}

int main(int argc, char **argv){
    int repeats = 1;
    int threads = 3;
    int opt;

    while ((opt = getopt(argc, argv, "r:t:")) != -1){
        switch (opt){
            case 'r': repeats = atoi(optarg); break;
            case 't': threads = atoi(optarg); break;
            default:
                fprintf(stderr, "Usage: %s [-r repeats] [-t threads] FILE...\n", argv[0]);
                return 1;
        }
    }
    if (optind >= argc || repeats < 1 || threads < 1){
        fprintf(stderr, "Usage: %s [-r repeats] [-t threads] FILE...\n", argv[0]);
        return 1;
    }

    printf("zlib %s, libdeflate %s, zstd %s\n", zlibVersion(), LIBDEFLATE_VERSION_STRING, ZSTD_versionString());

    int zstd_threads = zstd_has_threads();
    if (!zstd_threads){
        printf("libzstd built without multithreading: skipping the threaded zstd rows\n");
    }

    row_t rows[] = {
        {"zlib", 1, 1}, {"zlib", 6, 1},
        {"libdeflate", 1, 1}, {"libdeflate", 6, 1},
        {"zstd", 1, 1}, {"zstd", 3, 1}, {"zstd", 6, 1},
        {"zstd", 1, threads}, {"zstd", 3, threads},
    };
    size_t n_rows = sizeof(rows) / sizeof(rows[0]);

    for (int i = optind; i < argc; i++){
        size_t size = 0;
        unsigned char *in = read_file(argv[i], &size);
        if (!in){
            return 1;
        }
        unsigned char *restored = malloc(size ? size : 1);
        if (!restored){
            perror("malloc");
            return 1;
        }
        memset(restored, 0, size);

        printf("\n== %s (%.1f MB, best of %d)\n", argv[i], size / 1e6, repeats);
        printf("%-10s %5s %7s %9s %9s %6s %8s %9s %8s %9s\n",
            "codec", "level", "threads", "in_MB", "out_MB", "ratio",
            "comp_s", "comp_MB/s", "decomp_s", "decomp_MB/s");

        for (size_t r = 0; r < n_rows; r++){
            if (rows[r].threads > 1 && (!zstd_threads || strcmp(rows[r].codec, "zstd") != 0)){
                continue;
            }
            bench_row(&rows[r], in, size, restored, repeats);
        }

        free(restored);
        free(in);
    }
    return 0;
}
