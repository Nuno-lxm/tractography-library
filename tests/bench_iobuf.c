// Time streaming writes and reads of real images at different I/O buffer sizes.
//
//   -m gz    zlib's gz API with gzbuffer(size): system zlib, or zlib-ng when its
//            libz.so.1 is preloaded (the zlib line on stderr says which loaded)
//   -m zstd  zstd's streaming API: compressed bytes go out in write() calls of
//            the buffer size and come back in read() calls of the buffer size
//
// Each write is followed by a read of the same file, checked against the input.
// Sizes alternate inside every repetition, so drift in file system load spreads
// over all of them.
//
// Usage:
//   bench_iobuf -m gz|zstd -d OUTDIR [-r REPEATS] [-l LEVEL] [-c CHUNK]
//               [-b SIZE,SIZE,...] [-t VARIANT] [-w WHERE] [-p PROCS] [-s] [-x] FILE.nii...
//   -l  compression level (default 6 for gz, 3 for zstd)
//   -c  bytes per gzwrite/gzread or stream call (0 = whole image in one call, like ITK/MRtrix)
//   -b  buffer sizes in bytes (default 8K,32K,64K,128K,256K,1M,4M)
//   -t, -w, -p  labels copied into the CSV (variant name, storage location, process count)
//   -s  fsync after each write   -x  drop the file from the page cache before each read
//
// Output: CSV on stdout. For zstd the calls column counts the write()/read()
// syscalls the benchmark made; for gz they happen inside zlib, so it is -1.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>
#include <zstd.h>

#define MAX_SIZES 32
#define MAX_CALL (1u << 30)

static unsigned sizes[MAX_SIZES] = {8192, 32768, 65536, 131072, 262144, 1048576, 4194304};
static int nsizes = 7;
static long write_calls, read_calls;


/* Helpers */

static double now(void){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void die(const char *what, const char *detail){
    fprintf(stderr, "bench_iobuf: %s%s%s\n", what, detail ? ": " : "", detail ? detail : "");
    exit(1);
}

static unsigned char *load(const char *path, size_t *n){
    FILE *f = fopen(path, "rb");
    if (!f) die("cannot open input", path);
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    rewind(f);
    unsigned char *buf = malloc(*n ? *n : 1);
    if (!buf || fread(buf, 1, *n, f) != *n) die("cannot read input", path);
    fclose(f);
    return buf;
}

static size_t next_len(size_t n, size_t pos, size_t chunk){
    size_t len = (chunk && n - pos > chunk) ? chunk : n - pos;
    return len > MAX_CALL ? MAX_CALL : len;
}

static int write_all(int fd, const void *buf, size_t len){
    const unsigned char *ptr = buf;
    while (len > 0){
        ssize_t written = write(fd, ptr, len);
        if (written < 0){
            if (errno == EINTR) continue;
            return -1;
        }
        write_calls++;
        ptr += written;
        len -= written;
    }
    return 0;
}

// Clean pages only: write with -s first. Reliable on local disks, best effort on Lustre
static void drop_cache(const char *path){
    int fd = open(path, O_RDONLY);
    if (fd >= 0){
        posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
        close(fd);
    }
}

static long long file_size(const char *path){
    struct stat st;
    return stat(path, &st) == 0 ? (long long)st.st_size : -1;
}

static void parse_sizes(const char *list){
    char *copy = strdup(list), *save = NULL;
    nsizes = 0;
    for (char *p = strtok_r(copy, ",", &save); p && nsizes < MAX_SIZES; p = strtok_r(NULL, ",", &save)){
        char *end;
        unsigned long v = strtoul(p, &end, 10);
        if (*end == 'K' || *end == 'k') v <<= 10;
        else if (*end == 'M' || *end == 'm') v <<= 20;
        if (v < 2) die("buffer sizes must be at least 2 bytes", p);
        sizes[nsizes++] = (unsigned)v;
    }
    free(copy);
    if (nsizes == 0) die("no buffer sizes", list);
}


/* gz: zlib's own buffering, sized with gzbuffer */

static double gz_write(const char *out, const unsigned char *data, size_t n,
                       unsigned bufsz, int level, size_t chunk, int sync){
    char mode[16];
    snprintf(mode, sizeof(mode), "wb%d", level < 0 ? 0 : level > 9 ? 9 : level);   // zlib levels are 0-9
    double t0 = now();
    int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) die("cannot create", out);
    int keep = dup(fd);                       // gzclose closes fd; keep a copy for fsync
    gzFile g = gzdopen(fd, mode);
    if (!g) die("gzdopen failed", out);
    if (gzbuffer(g, bufsz) != 0) die("gzbuffer failed", out);
    for (size_t pos = 0; pos < n; ){
        size_t len = next_len(n, pos, chunk);
        if (gzwrite(g, data + pos, (unsigned)len) <= 0) die("gzwrite failed", out);
        pos += len;
    }
    if (gzclose(g) != Z_OK) die("gzclose failed", out);
    if (sync) fsync(keep);
    close(keep);
    return now() - t0;
}

static double gz_read(const char *in, unsigned char *dst, size_t n, unsigned bufsz, size_t chunk){
    double t0 = now();
    gzFile g = gzopen(in, "rb");
    if (!g) die("gzopen failed", in);
    if (gzbuffer(g, bufsz) != 0) die("gzbuffer failed", in);
    size_t pos = 0;
    while (pos < n){
        int got = gzread(g, dst + pos, (unsigned)next_len(n, pos, chunk));
        if (got <= 0) break;
        pos += got;
    }
    gzclose(g);
    double t = now() - t0;
    if (pos != n) die("short read", in);
    return t;
}


/* zstd: the benchmark owns the buffers and the syscalls */

// The output buffer is written only when full (and once at the end), so every
// write() but the last is exactly bufsz bytes
static double zstd_write(ZSTD_CCtx *cctx, const char *out, const unsigned char *data, size_t n,
                         unsigned bufsz, int level, size_t chunk, int sync){
    unsigned char *obuf = malloc(bufsz);
    if (!obuf) die("out of memory", NULL);
    double t0 = now();
    int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) die("cannot create", out);

    ZSTD_CCtx_reset(cctx, ZSTD_reset_session_only);
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, level);
    ZSTD_outBuffer o = {obuf, bufsz, 0};

    size_t pos = 0;
    for (;;){
        size_t len = pos < n ? next_len(n, pos, chunk) : 0;
        ZSTD_EndDirective mode = pos + len >= n ? ZSTD_e_end : ZSTD_e_continue;
        ZSTD_inBuffer in = {data + pos, len, 0};
        size_t remaining;
        do {
            remaining = ZSTD_compressStream2(cctx, &o, &in, mode);
            if (ZSTD_isError(remaining)) die("ZSTD_compressStream2", ZSTD_getErrorName(remaining));
            if (o.pos == o.size){
                if (write_all(fd, obuf, o.pos) != 0) die("write failed", out);
                o.pos = 0;
            }
        } while (mode == ZSTD_e_end ? remaining != 0 : in.pos < in.size);
        pos += len;
        if (mode == ZSTD_e_end) break;
    }
    if (o.pos && write_all(fd, obuf, o.pos) != 0) die("write failed", out);
    if (sync) fsync(fd);
    close(fd);
    double t = now() - t0;
    free(obuf);
    return t;
}

// Compressed bytes come in with read() calls of bufsz and are decompressed
// straight into the destination, like a gzread caller's buffer
static double zstd_read(ZSTD_DCtx *dctx, const char *in, unsigned char *dst, size_t n,
                        unsigned bufsz, size_t chunk){
    unsigned char *ibuf = malloc(bufsz);
    if (!ibuf) die("out of memory", NULL);
    double t0 = now();
    int fd = open(in, O_RDONLY);
    if (fd < 0) die("cannot open", in);
    ZSTD_DCtx_reset(dctx, ZSTD_reset_session_only);

    ZSTD_inBuffer ib = {ibuf, 0, 0};
    size_t pos = 0;
    int eof = 0, stalled = 0;
    while (pos < n && !stalled){
        ZSTD_outBuffer ob = {dst + pos, next_len(n, pos, chunk), 0};
        while (ob.pos < ob.size){
            if (ib.pos == ib.size && !eof){
                ssize_t got = read(fd, ibuf, bufsz);
                if (got < 0){
                    if (errno == EINTR) continue;
                    die("read failed", in);
                }
                read_calls++;
                if (got == 0) eof = 1;
                ib.size = (size_t)got;
                ib.pos = 0;
            }
            size_t before = ob.pos;
            size_t ret = ZSTD_decompressStream(dctx, &ob, &ib);
            if (ZSTD_isError(ret)) die("ZSTD_decompressStream", ZSTD_getErrorName(ret));
            if (eof && ib.pos == ib.size && ob.pos == before){
                stalled = 1;
                break;
            }
        }
        pos += ob.pos;
    }
    close(fd);
    double t = now() - t0;
    free(ibuf);
    if (pos != n) die("short read", in);
    return t;
}


int main(int argc, char **argv){
    const char *mode = NULL, *outdir = NULL, *variant = "", *where = "", *procs = "1";
    int repeats = 3, level = -1, sync = 0, drop = 0, opt;
    size_t chunk = 0;
    while ((opt = getopt(argc, argv, "m:d:r:l:c:b:t:w:p:sx")) != -1){
        switch (opt){
        case 'm': mode = optarg; break;
        case 'd': outdir = optarg; break;
        case 'r': repeats = atoi(optarg); break;
        case 'l': level = atoi(optarg); break;
        case 'c': chunk = strtoull(optarg, NULL, 10); break;
        case 'b': parse_sizes(optarg); break;
        case 't': variant = optarg; break;
        case 'w': where = optarg; break;
        case 'p': procs = optarg; break;
        case 's': sync = 1; break;
        case 'x': drop = 1; break;
        default: return 1;
        }
    }
    int use_zstd = mode && strcmp(mode, "zstd") == 0;
    if (!mode || (!use_zstd && strcmp(mode, "gz") != 0) || !outdir || optind >= argc){
        fprintf(stderr, "Usage: %s -m gz|zstd -d OUTDIR [-r N] [-l LEVEL] [-c CHUNK] [-b SIZES] "
                        "[-t VARIANT] [-w WHERE] [-p PROCS] [-s] [-x] FILE.nii...\n", argv[0]);
        return 1;
    }
    if (level < 0) level = use_zstd ? 3 : 6;
    const char *lib = use_zstd ? ZSTD_versionString() : zlibVersion();
    fprintf(stderr, "%s %s, level %d, chunk %zu, fsync %d, drop cache %d\n",
            use_zstd ? "zstd" : "zlib", lib, level, chunk, sync, drop);

    ZSTD_CCtx *cctx = use_zstd ? ZSTD_createCCtx() : NULL;
    ZSTD_DCtx *dctx = use_zstd ? ZSTD_createDCtx() : NULL;
    if (use_zstd && (!cctx || !dctx)) die("cannot create zstd contexts", NULL);

    printf("variant,where,procs,mode,lib,file,op,level,bufsize,chunk,run,seconds,MBps,calls,compressed_bytes\n");
    for (int f = optind; f < argc; f++){
        size_t n;
        unsigned char *data = load(argv[f], &n), *back = malloc(n ? n : 1);
        if (!back) die("out of memory", NULL);
        char *copy = strdup(argv[f]), *name = basename(copy), out[4096];
        snprintf(out, sizeof(out), "%s/%s.bench.%s", outdir, name, use_zstd ? "zst" : "gz");

        for (int r = 0; r < repeats; r++){
            for (int i = 0; i < nsizes; i++){
                write_calls = 0;
                double w = use_zstd ? zstd_write(cctx, out, data, n, sizes[i], level, chunk, sync)
                                    : gz_write(out, data, n, sizes[i], level, chunk, sync);
                long wcalls = use_zstd ? write_calls : -1;
                long long csize = file_size(out);

                if (drop) drop_cache(out);
                read_calls = 0;
                double rd = use_zstd ? zstd_read(dctx, out, back, n, sizes[i], chunk)
                                     : gz_read(out, back, n, sizes[i], chunk);
                long rcalls = use_zstd ? read_calls : -1;
                if (memcmp(data, back, n) != 0) die("round trip mismatch", name);

                printf("%s,%s,%s,%s,%s,%s,write,%d,%u,%zu,%d,%.4f,%.1f,%ld,%lld\n",
                       variant, where, procs, mode, lib, name, level, sizes[i], chunk, r, w, n / w / 1e6, wcalls, csize);
                printf("%s,%s,%s,%s,%s,%s,read,%d,%u,%zu,%d,%.4f,%.1f,%ld,%lld\n",
                       variant, where, procs, mode, lib, name, level, sizes[i], chunk, r, rd, n / rd / 1e6, rcalls, csize);
                fflush(stdout);
            }
        }
        unlink(out);
        free(copy);
        free(data);
        free(back);
    }
    ZSTD_freeCCtx(cctx);
    ZSTD_freeDCtx(dctx);
    return 0;
}
