// Round-trip tests for the streaming zstd_lib (src/zstd_lib.c). They call
// zlib's gz API and need the library preloaded:
//
//   make zstd_lib.so test_zstd_stream
//   LD_PRELOAD=$PWD/bin/zstd_lib.so ./bin/test_zstd_stream
//
// Files go to ./test/zstd_stream/. The test sets ZSTD_FILE_TARGETS itself:
// basenames starting with z_ become zstd, g_ files stay gzip. The library's
// stderr lines go to test/zstd_stream/zstd_lib.log, which is also where the
// rewind counts are read from. Every zstd file is also decoded by the zstd
// command-line tool when it is installed. SKIP_4GB=1 skips the file over 4 GB.
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <zlib.h>

#define DIR "test/zstd_stream"
#define LOG DIR "/zstd_lib.log"
#define MB (1024 * 1024)

static int checks, failures;
static int cli;             // zstd command-line tool installed

#define CHECK(cond) do{ \
    checks++; \
    if (!(cond)){ \
        printf("    FAIL line %d: %s\n", __LINE__, #cond); \
        failures++; \
    } \
} while (0)


/* Helpers */

// Path of a file in DIR; a few calls can be used at once
static const char *path(const char *name){
    static char bufs[8][256];
    static int i;
    char *p = bufs[i++ % 8];
    snprintf(p, sizeof(bufs[0]), "%s/%s", DIR, name);
    return p;
}

// Byte at offset i of the test data: 16 random symbols (splitmix64), so it
// compresses to a bit over half, and any offset can be checked without the
// data at hand
static unsigned char at(long long i){
    uint64_t x = (uint64_t)i + 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return 'a' + ((x ^ (x >> 31)) & 15);
}

static unsigned char *make_data(size_t n){
    unsigned char *data = malloc(n ? n : 1);
    for (size_t i = 0; i < n; i++){
        data[i] = at(i);
    }
    return data;
}

static int same_as_at(const unsigned char *buf, long long from, size_t n){
    for (size_t i = 0; i < n; i++){
        if (buf[i] != at(from + i))
            return 0;
    }
    return 1;
}

// 'z' for zstd, 'g' for gzip, 0 for anything else
static int magic(const char *file){
    unsigned char m[4] = {0};
    int fd = open(file, O_RDONLY);
    ssize_t got = fd >= 0 ? read(fd, m, 4) : -1;
    if (fd >= 0) close(fd);
    if (got == 4 && m[0] == 0x28 && m[1] == 0xb5 && m[2] == 0x2f && m[3] == 0xfd) return 'z';
    if (got >= 2 && m[0] == 0x1f && m[1] == 0x8b) return 'g';
    return 0;
}

static long long file_size(const char *file){
    struct stat st;
    return stat(file, &st) == 0 ? st.st_size : -1;
}

// Writes data with gzwrite calls of chunk bytes
static int write_gz(const char *file, const char *mode, const unsigned char *data, size_t n, size_t chunk){
    gzFile g = gzopen(file, mode);
    if (!g) return -1;
    for (size_t off = 0; off < n; off += chunk){
        unsigned len = n - off < chunk ? n - off : chunk;
        if (gzwrite(g, data + off, len) != (int)len){
            gzclose(g);
            return -1;
        }
    }
    return gzclose(g) == Z_OK ? 0 : -1;
}

// Reads the whole file with gzread calls of chunk bytes. Returns the size,
// or -1
static long long read_gz(const char *file, unsigned char *dst, size_t cap, size_t chunk){
    gzFile g = gzopen(file, "rb");
    if (!g) return -1;
    size_t total = 0;
    for (;;){
        unsigned len = cap - total < chunk ? cap - total : chunk;
        if (len == 0) break;
        int got = gzread(g, dst + total, len);
        if (got < 0){
            gzclose(g);
            return -1;
        }
        total += got;
        if (got == 0) break;
    }
    gzclose(g);
    return total;
}

static int copy_bytes(const char *from, const char *to, long long skip_tail, const char *prefix){
    long long n = file_size(from) - skip_tail;
    unsigned char *buf = malloc(n > 0 ? n : 1);
    int in = open(from, O_RDONLY);
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int ok = buf && in >= 0 && out >= 0 && read(in, buf, n) == n &&
        (!prefix || write(out, prefix, strlen(prefix)) == (ssize_t)strlen(prefix)) && write(out, buf, n) == n;
    if (in >= 0) close(in);
    if (out >= 0) close(out);
    free(buf);
    return ok ? 0 : -1;
}

// Flips the lowest bit of the byte at offset (from the end when negative)
static int flip_byte(const char *file, long long offset){
    unsigned char c;
    int fd = open(file, O_RDWR);
    if (offset < 0) offset += file_size(file);
    int ok = fd >= 0 && pread(fd, &c, 1, offset) == 1;
    c ^= 1;
    ok = ok && pwrite(fd, &c, 1, offset) == 1;
    if (fd >= 0) close(fd);
    return ok ? 0 : -1;
}

// Reads the whole file in 64 KB calls; returns the last gzread result (0 at
// the end, -1 on an error), with gzerror's code in *err and message in msg
static int read_to_end(const char *file, unsigned char *buf, int *err, char msg[128]){
    gzFile r = gzopen(file, "rb");
    int got = -2;
    if (r){
        while ((got = gzread(r, buf, 65536)) > 0);
        snprintf(msg, 128, "%s", gzerror(r, err));
        gzclose(r);
    }
    return got;
}

// The zstd tool decodes file to exactly data (always true without the tool)
static int cli_matches(const char *file, const unsigned char *data, size_t n){
    if (!cli) return 1;
    char out[300], cmd[700];
    snprintf(out, sizeof(out), "%s.cli", file);
    snprintf(cmd, sizeof(cmd), "LD_PRELOAD= zstd -dcq '%s' > '%s' 2>/dev/null", file, out);
    int ok = system(cmd) == 0 && file_size(out) == (long long)n;
    if (ok){
        unsigned char *buf = malloc(n ? n : 1);
        int fd = open(out, O_RDONLY);
        ok = fd >= 0 && read(fd, buf, n) == (ssize_t)n && memcmp(buf, data, n) == 0;
        if (fd >= 0) close(fd);
        free(buf);
    }
    unlink(out);
    return ok;
}

// Closes a read handle and returns the rewind count from its close line
static int close_rewinds(gzFile g){
    off_t before = lseek(STDERR_FILENO, 0, SEEK_END);
    gzclose(g);
    char line[512] = {0};
    int fd = open(LOG, O_RDONLY);
    ssize_t got = fd >= 0 ? pread(fd, line, sizeof(line) - 1, before) : -1;
    if (fd >= 0) close(fd);
    int rewinds = -1;
    const char *p = got > 0 ? strstr(line, "zstd_lib: read,") : NULL;
    if (p && sscanf(p, "zstd_lib: read, %*d -> %*d bytes, %d rewinds", &rewinds) != 1){
        rewinds = -1;
    }
    return rewinds;
}

// Reads n bytes at the current offset and compares them with at()
static int read_matches(gzFile g, long long from, size_t n){
    unsigned char buf[4096];
    if (n > sizeof(buf)) return 0;
    return gzread(g, buf, n) == (int)n && same_as_at(buf, from, n);
}


/* Tests */

static void test_large_write(void){
    size_t n = 20 * MB;
    unsigned char *data = make_data(n);
    unsigned char *back = malloc(n + 1);
    const char *f = path("z_large.gz");

    CHECK(write_gz(f, "wb", data, n, n) == 0);
    CHECK(magic(f) == 'z');
    CHECK(file_size(f) < (long long)n * 3 / 4);
    // One read larger than the file
    CHECK(read_gz(f, back, n + 1, n + 1) == (long long)n);
    CHECK(memcmp(back, data, n) == 0);
    CHECK(cli_matches(f, data, n));
    free(data);
    free(back);
}

static void test_small_writes(void){
    static const size_t writes[] = {1, 8192, 3, 8191, 1, 65537, 8193, 7, 131072, 131073, 1};
    static const size_t reads[] = {1, 13, 8192, 4095, 1, 131072, 200000, 3, 131071};
    size_t n = 3 * MB + 12345;
    unsigned char *data = make_data(n);
    unsigned char *back = malloc(n);
    const char *f = path("z_small.gz");

    gzFile g = gzopen(f, "wb");
    CHECK(g != NULL);
    int ok = 1;
    for (size_t off = 0, i = 0; off < n; i++){
        size_t len = writes[i % 11] < n - off ? writes[i % 11] : n - off;
        ok &= gzwrite(g, data + off, len) == (int)len;
        off += len;
    }
    CHECK(ok);
    CHECK(gztell(g) == (z_off_t)n);
    CHECK(gzclose(g) == Z_OK);

    // Odd-sized reads, then only 1-byte reads
    g = gzopen(f, "rb");
    size_t total = 0;
    for (size_t i = 0; total < n; i++){
        int got = gzread(g, back + total, reads[i % 9]);
        if (got <= 0) break;
        total += got;
    }
    CHECK(total == n && memcmp(back, data, n) == 0);
    CHECK(gzread(g, back, 1) == 0 && gzeof(g));
    gzclose(g);

    g = gzopen(f, "rb");
    ok = 1;
    for (size_t i = 0; i < n; i++){
        unsigned char c;
        ok &= gzread(g, &c, 1) == 1 && c == data[i];
    }
    CHECK(ok);
    CHECK(!gzeof(g));
    CHECK(gzread(g, back, 1) == 0 && gzeof(g));
    gzclose(g);
    CHECK(cli_matches(f, data, n));
    free(data);
    free(back);
}

static void test_text_and_flush(void){
    const char *f = path("z_text.gz");
    size_t cap = 256 * 1024, len = 0;
    char *expect = malloc(cap);
    char *big = malloc(20001);
    memset(big, 'B', 20000);
    big[20000] = '\0';

    gzFile g = gzopen(f, "wb");
    CHECK(gzputs(g, "header line\n") == 12);
    len += sprintf(expect + len, "header line\n");
    int ok = 1;
    for (int i = 0; i < 1000; i++){
        int n = sprintf(expect + len, "line %d %s\n", i, i % 7 ? "abc" : "");
        ok &= gzprintf(g, "line %d %s\n", i, i % 7 ? "abc" : "") == n;
        len += n;
    }
    CHECK(ok);
    CHECK(gzputc(g, 'X') == 'X' && gzputc(g, '\n') == '\n');
    len += sprintf(expect + len, "X\n");
    // Longer than zlib's printf buffer
    CHECK(gzprintf(g, "%s\n", big) == 20001);
    len += sprintf(expect + len, "%s\n", big);

    long long before = file_size(f);
    CHECK(gzflush(g, Z_SYNC_FLUSH) == Z_OK);
    CHECK(file_size(f) > before);

    // A reader gets everything flushed, then an error for the unfinished frame
    gzFile r = gzopen(f, "rb");
    CHECK(r != NULL);
    char *back = malloc(cap);
    CHECK(gzread(r, back, len) == (int)len && memcmp(back, expect, len) == 0);
    CHECK(gzread(r, back, 1) == -1);
    int err;
    const char *msg = gzerror(r, &err);
    CHECK(err == Z_DATA_ERROR && strstr(msg, "unexpected end of file"));

    CHECK(gzputs(g, "after flush\n") == 12);
    len += sprintf(expect + len, "after flush\n");
    CHECK(gzclose(g) == Z_OK);

    // As with zlib, gzclearerr lets the reader go on once the file has grown
    gzclearerr(r);
    CHECK(gzread(r, back, 100) == 12 && memcmp(back, "after flush\n", 12) == 0);
    CHECK(gzeof(r));
    CHECK(gztell(r) == (z_off_t)len);
    CHECK(gzclose(r) == Z_OK);
    CHECK(cli_matches(f, (unsigned char *)expect, len));
    free(back);
    free(big);
    free(expect);
}

static void test_gets_getc_ungetc(void){
    const char *f = path("z_lines.gz");
    char text[1024];
    char longline[302];
    memset(longline, 'x', 300);
    longline[300] = '\n';
    longline[301] = '\0';
    int n = snprintf(text, sizeof(text), "alpha\nbravo charlie\n%slast line", longline);
    CHECK(write_gz(f, "wb", (unsigned char *)text, n, n) == 0);

    gzFile r = gzopen(f, "rb");
    char buf[64];
    CHECK(gzgets(r, buf, sizeof(buf)) && strcmp(buf, "alpha\n") == 0);
    CHECK(gztell(r) == 6);
    CHECK(gzgets(r, buf, 1) == NULL);
    CHECK(gzgetc(r) == 'b');            // zlib.h's macro
    CHECK((gzgetc)(r) == 'r');          // the function
    CHECK(gzgetc_(r) == 'a');
    CHECK(gzungetc('a', r) == 'a');
    CHECK(gztell(r) == 8);
    CHECK(gzungetc('Q', r) == 'Q');     // needn't be what was read
    CHECK(gzgetc(r) == 'Q' && gzgetc(r) == 'a' && gzgetc(r) == 'v');
    CHECK(gzungetc('Z', r) == 'Z');
    CHECK(gzgets(r, buf, sizeof(buf)) && strcmp(buf, "Zo charlie\n") == 0);

    // A line longer than the buffer comes in pieces of len - 1
    char joined[400] = "";
    int pieces = 0;
    while (gzgets(r, buf, sizeof(buf))){
        pieces++;
        strcat(joined, buf);
        if (buf[strlen(buf) - 1] == '\n') break;
    }
    CHECK(pieces == 5 && strcmp(joined, longline) == 0);

    CHECK(!gzeof(r));
    CHECK(gzgets(r, buf, sizeof(buf)) && strcmp(buf, "last line") == 0);
    CHECK(gzeof(r));
    CHECK(gzgets(r, buf, sizeof(buf)) == NULL);
    CHECK(gzgetc(r) == -1);
    CHECK(gzungetc('!', r) == '!' && !gzeof(r));
    CHECK(gzgetc(r) == '!');
    CHECK(gztell(r) == n);
    CHECK(gzclose(r) == Z_OK);
}

static void test_seeks(void){
    size_t n = 5 * MB;
    unsigned char *data = make_data(n);
    const char *f = path("z_seek.gz");
    CHECK(write_gz(f, "wb", data, n, MB) == 0);
    unsigned char buf[4096];

    for (int cache = 1; cache >= 0; cache--){
        printf("    header cache %s\n", cache ? "1 MB" : "off");
        if (cache) unsetenv("ZSTD_HEADER_CACHE");
        else setenv("ZSTD_HEADER_CACHE", "0", 1);

        gzFile r = gzopen(f, "rb");
        CHECK(gzseek(r, 1000, SEEK_SET) == 1000 && read_matches(r, 1000, 100));
        CHECK(gzseek(r, 50000, SEEK_CUR) == 51100 && read_matches(r, 51100, 100));
        // Back inside the first decoded chunk (and the cache)
        CHECK(gzseek(r, -40000, SEEK_CUR) == 11200 && read_matches(r, 11200, 100));
        CHECK(gztell(r) == 11300);
        // Forward past the cache: decoded and dropped
        CHECK(gzseek(r, 3 * MB, SEEK_SET) == 3 * MB && read_matches(r, 3 * MB, 100));
        // A few bytes back stay in the last decoded chunk
        CHECK(gzseek(r, -50, SEEK_CUR) == 3 * MB + 50 && read_matches(r, 3 * MB + 50, 100));
        // Back past the cache and the chunk: decoded again from the start
        CHECK(gzseek(r, -MB, SEEK_CUR) == 2 * MB + 150 && read_matches(r, 2 * MB + 150, 100));
        // The header again: free with the cache, a restart without it
        CHECK(gzrewind(r) == 0 && gztell(r) == 0 && read_matches(r, 0, 348));
        CHECK(gzseek64(r, 352, SEEK_SET) == 352 && read_matches(r, 352, 1000));

        // Past the end: the seek succeeds, the read finds the end
        CHECK(gzseek(r, n + 1000, SEEK_SET) == (z_off_t)(n + 1000));
        CHECK(gzread(r, buf, 10) == 0 && gzeof(r));
        CHECK(gztell(r) == (z_off_t)n);
        CHECK(gzseek(r, -1, SEEK_CUR) == (z_off_t)(n - 1) && !gzeof(r) && read_matches(r, n - 1, 1));
        CHECK(gzseek(r, 0, SEEK_END) == -1);
        CHECK(gzseek(r, -1, SEEK_SET) == -1);
        CHECK(gzseek(r, -(z_off_t)n - 1, SEEK_CUR) == -1);
        int rewinds = close_rewinds(r);
        printf("    %d rewinds\n", rewinds);
        CHECK(rewinds == (cache ? 1 : 2));

        // FSL's pattern: header, rewind, header again, then all the data
        r = gzopen(f, "rb");
        unsigned char *back = malloc(n);
        CHECK(gzread(r, back, 348) == 348 && gzrewind(r) == 0);
        CHECK(gzread(r, back, 352) == 352 && gzseek(r, 352, SEEK_SET) == 352);
        CHECK(gzread(r, back + 352, n) == (int)(n - 352) && memcmp(back, data, n) == 0);
        CHECK(close_rewinds(r) == 0);

        // Read past the first chunk, then rewind: only the cache avoids a restart
        r = gzopen(f, "rb");
        CHECK(gzread(r, back, MB / 2) == MB / 2 && gzrewind(r) == 0 && read_matches(r, 0, 4096));
        CHECK(close_rewinds(r) == (cache ? 0 : 1));
        free(back);
    }
    unsetenv("ZSTD_HEADER_CACHE");

    // Writes: forward seeks compress zeros, backward ones fail
    const char *w = path("z_wseek.gz");
    gzFile g = gzopen(w, "wb");
    CHECK(gzwrite(g, data, 100) == 100);
    CHECK(gzseek(g, 1000, SEEK_CUR) == 1100);
    CHECK(gzwrite(g, data + 100, 10) == 10);
    CHECK(gzseek(g, 5, SEEK_SET) == -1 && gzseek(g, 0, SEEK_END) == -1);
    CHECK(gzseek(g, 0, SEEK_CUR) == 1110 && gztell(g) == 1110);
    CHECK(gzclose(g) == Z_OK);
    unsigned char expect[1110] = {0};
    memcpy(expect, data, 100);
    memcpy(expect + 1100, data + 100, 10);
    unsigned char wback[2000];
    CHECK(read_gz(w, wback, sizeof(wback), sizeof(wback)) == 1110 && memcmp(wback, expect, 1110) == 0);
    CHECK(cli_matches(w, expect, 1110));
    free(data);
}

static void test_concatenated(void){
    size_t n = 700 * 1024;
    unsigned char *data = make_data(n);
    unsigned char *back = malloc(n + 1);

    // gzflush(Z_FINISH) ends a frame; the next write starts another. The
    // second flush in a row adds an empty frame
    const char *f = path("z_frames.gz");
    gzFile g = gzopen(f, "wb");
    CHECK(gzwrite(g, data, 300000) == 300000);
    CHECK(gzflush(g, Z_FINISH) == Z_OK && gzflush(g, Z_FINISH) == Z_OK);
    CHECK(gzwrite(g, data + 300000, 200000) == 200000);
    CHECK(gzflush(g, Z_FINISH) == Z_OK);
    CHECK(gzwrite(g, data + 500000, n - 500000) == (int)(n - 500000));
    CHECK(gzclose(g) == Z_OK);
    CHECK(read_gz(f, back, n + 1, 77777) == (long long)n && memcmp(back, data, n) == 0);
    CHECK(cli_matches(f, data, n));

    // Two files joined byte for byte, then a seek back across the frames
    CHECK(write_gz(path("z_part1.gz"), "wb", data, 400000, 400000) == 0);
    CHECK(write_gz(path("z_part2.gz"), "wb", data + 400000, n - 400000, 65536) == 0);
    const char *cat = path("z_cat.gz");
    CHECK(copy_bytes(path("z_part1.gz"), cat, 0, NULL) == 0);
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "cat '%s' >> '%s'", path("z_part2.gz"), cat);
    CHECK(system(cmd) == 0);
    CHECK(read_gz(cat, back, n + 1, n + 1) == (long long)n && memcmp(back, data, n) == 0);
    setenv("ZSTD_HEADER_CACHE", "0", 1);
    gzFile r = gzopen(cat, "rb");
    CHECK(gzseek(r, 600000, SEEK_SET) == 600000 && read_matches(r, 600000, 1000));
    CHECK(gzseek(r, 350000, SEEK_SET) == 350000 && read_matches(r, 350000, 4096));
    CHECK(close_rewinds(r) == 1);
    unsetenv("ZSTD_HEADER_CACHE");
    CHECK(cli_matches(cat, data, n));
    free(data);
    free(back);
}

static void test_truncated(void){
    size_t n = 600 * 1024;
    unsigned char *data = make_data(n);
    unsigned char *back = malloc(n);
    const char *f = path("z_whole.gz");
    const char *cut = path("z_cut.gz");
    CHECK(write_gz(f, "wb", data, n, n) == 0);

    // Cut 50 bytes short: the data before the cut, then an error
    CHECK(copy_bytes(f, cut, 50, NULL) == 0);
    gzFile r = gzopen(cut, "rb");
    long long total = 0;
    int got;
    while ((got = gzread(r, back + total, 65536)) > 0){
        total += got;
    }
    int err;
    gzerror(r, &err);
    CHECK(got == -1 && err == Z_DATA_ERROR);
    CHECK(total < (long long)n && memcmp(back, data, total) == 0);
    CHECK(gzread(r, back, 1) == -1);
    CHECK(gzclose(r) == Z_DATA_ERROR);

    // Every frame has a checksum (bit 2 of the frame header descriptor): a
    // damaged checksum, or a damaged byte in the data, fails the read
    unsigned char hdr[5] = {0};
    int fd = open(f, O_RDONLY);
    CHECK(fd >= 0 && read(fd, hdr, 5) == 5 && (hdr[4] & 4));
    close(fd);
    const char *bad = path("z_bad.gz");
    CHECK(copy_bytes(f, bad, 0, NULL) == 0 && flip_byte(bad, -1) == 0);
    char msg[128] = "";
    CHECK(read_to_end(bad, back, &err, msg) == -1 && err == Z_DATA_ERROR);
    CHECK(strstr(msg, "checksum") != NULL);
    CHECK(copy_bytes(f, bad, 0, NULL) == 0 && flip_byte(bad, file_size(f) / 2) == 0);
    CHECK(read_to_end(bad, back, &err, msg) == -1 && err == Z_DATA_ERROR);
    printf("    damaged data: %s\n", strchr(msg, ':') ? strchr(msg, ':') + 2 : msg);

    // Only the magic number left: still opened as zstd, fails on the read
    fd = open(cut, O_WRONLY | O_TRUNC);
    CHECK(fd >= 0 && write(fd, "\x28\xb5\x2f\xfd", 4) == 4);
    close(fd);
    r = gzopen(cut, "rb");
    CHECK(r != NULL && gzread(r, back, 10) == -1 && gzgetc(r) == -1);
    gzclose(r);
    free(data);
    free(back);
}

static void test_empty(void){
    const char *f = path("z_empty.gz");
    gzFile g = gzopen(f, "wb");
    CHECK(g != NULL && gzclose(g) == Z_OK);
    CHECK(magic(f) == 'z');

    unsigned char buf[16];
    gzFile r = gzopen(f, "rb");
    CHECK(r != NULL && !gzdirect(r));
    CHECK(gzread(r, buf, sizeof(buf)) == 0 && gzeof(r));
    CHECK(gzgetc(r) == -1 && gztell(r) == 0);
    CHECK(gzclose(r) == Z_OK);
    CHECK(cli_matches(f, buf, 0));

    // A 0-byte file isn't zstd: zlib reads it, as an empty plain file
    const char *zero = path("g_zero.gz");
    close(open(zero, O_WRONLY | O_CREAT | O_TRUNC, 0644));
    r = gzopen(zero, "rb");
    CHECK(r != NULL && gzread(r, buf, sizeof(buf)) == 0 && gzeof(r));
    gzclose(r);
}

static void test_over_4gb(void){
    if (getenv("SKIP_4GB")){
        printf("    skipped (SKIP_4GB)\n");
        return;
    }
    const long long big = 4500000000LL;
    const char *f = path("z_4gb.gz");
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    gzFile g = gzopen64(f, "wb");
    CHECK(gzwrite(g, "HEAD", 4) == 4);
    CHECK(gzseek64(g, big, SEEK_SET) == big);
    CHECK(gzwrite(g, "TAIL", 4) == 4);
    CHECK(gztell64(g) == big + 4);
    CHECK(gzclose(g) == Z_OK);
    CHECK(file_size(f) < 10 * MB);

    char buf[16];
    gzFile r = gzopen64(f, "rb");
    CHECK(gzread(r, buf, 4) == 4 && memcmp(buf, "HEAD", 4) == 0);
    CHECK(gzseek64(r, big - 2, SEEK_SET) == big - 2);
    CHECK(gzread(r, buf, 16) == 6 && memcmp(buf, "\0\0TAIL", 6) == 0);
    CHECK(gzeof(r) && gztell64(r) == big + 4);
    CHECK(gzoffset64(r) == file_size(f));
    CHECK(gzseek64(r, -3500000000LL, SEEK_CUR) == 1000000004LL);
    CHECK(gzread(r, buf, 4) == 4 && memcmp(buf, "\0\0\0\0", 4) == 0);
    CHECK(gztell64(r) == 1000000008LL);
    CHECK(close_rewinds(r) == 1);

    if (cli){
        char cmd[400];
        snprintf(cmd, sizeof(cmd), "LD_PRELOAD= zstd -tq '%s'", f);
        CHECK(system(cmd) == 0);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("    %.1f s\n", t1.tv_sec - t0.tv_sec + (t1.tv_nsec - t0.tv_nsec) / 1e9);
}

static void test_gzip_and_raw(void){
    size_t n = 3 * MB;
    unsigned char *data = make_data(n);
    unsigned char *back = malloc(n + 1);

    // Not a target: standard gzip, read back through zlib
    const char *gz = path("g_plain.gz");
    CHECK(write_gz(gz, "wb", data, n, 65536) == 0);
    CHECK(magic(gz) == 'g');
    CHECK(read_gz(gz, back, n + 1, 100000) == (long long)n && memcmp(back, data, n) == 0);

    // A plain file: zlib's transparent reading
    const char *raw = path("g_raw.txt");
    int fd = open(raw, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(write(fd, data, 100000) == 100000);
    close(fd);
    gzFile r = gzopen(raw, "rb");
    CHECK(r != NULL && gzread(r, back, n) == 100000 && memcmp(back, data, 100000) == 0);
    CHECK(gzdirect(r) == 1);
    gzclose(r);

    // gzdopen reads zstd from the descriptor's offset, and restarts there
    const char *z = path("z_dopen.gz");
    const char *offset = path("z_dopen_offset.gz");
    CHECK(write_gz(z, "wb", data, n, n) == 0);
    char prefix[101];
    memset(prefix, '.', 100);
    prefix[100] = '\0';
    CHECK(copy_bytes(z, offset, 0, prefix) == 0);
    fd = open(offset, O_RDONLY);
    CHECK(lseek(fd, 100, SEEK_SET) == 100);
    r = gzdopen(fd, "rb");
    CHECK(r != NULL && !gzdirect(r));
    CHECK(gzread(r, back, 2 * MB) == 2 * MB && memcmp(back, data, 2 * MB) == 0);
    CHECK(gzrewind(r) == 0 && gzread(r, back, n + 1) == (int)n && memcmp(back, data, n) == 0);
    CHECK(close_rewinds(r) == 1);
    CHECK(fcntl(fd, F_GETFD) == -1);   // gzclose closed it

    // gzip through gzdopen stays with zlib, and gzdopen writes are gzip
    fd = open(gz, O_RDONLY);
    r = gzdopen(fd, "rb");
    CHECK(r != NULL && gzread(r, back, n + 1) == (int)n && memcmp(back, data, n) == 0);
    gzclose(r);
    const char *dw = path("z_dopen_write.gz");
    fd = open(dw, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    gzFile g = gzdopen(fd, "wb");
    CHECK(g != NULL && gzwrite(g, data, 1000) == 1000 && gzclose(g) == Z_OK);
    CHECK(magic(dw) == 'g');

    // Frames from the zstd tool (content size and checksum in the header, as
    // the one-shot library wrote them) read the same way
    if (cli){
        const char *cz = path("g_cli.gz");
        char cmd[400];
        snprintf(cmd, sizeof(cmd), "LD_PRELOAD= zstd -qf -T2 -o '%s' '%s'", cz, raw);
        CHECK(system(cmd) == 0 && magic(cz) == 'z');
        CHECK(read_gz(cz, back, n + 1, 4096) == 100000 && memcmp(back, data, 100000) == 0);
    }

    // Appends to a target stay with zlib
    const char *ap = path("z_append.gz");
    unlink(ap);
    CHECK(write_gz(ap, "ab", data, 1000, 1000) == 0 && magic(ap) == 'g');
    free(data);
    free(back);
}

static void test_api(void){
    size_t n = MB;
    unsigned char *data = make_data(n);
    unsigned char *back = malloc(n + 64);
    const char *f = path("z_api.gz");
    int err;

    // gzbuffer sizes every write(): the file grows in whole buffers until
    // gzclose. Only allowed before the first I/O
    gzFile g = gzopen(f, "wb");
    CHECK(gzbuffer(g, 4096) == 0);
    CHECK(gzfwrite(data, 8, n / 8, g) == n / 8);
    CHECK(gzbuffer(g, 8192) == -1);
    CHECK(gzoffset(g) > 0 && gzoffset(g) % 4096 == 0);
    CHECK(gzsetparams(g, 9, Z_DEFAULT_STRATEGY) == Z_OK);
    CHECK(gzdirect(g) == 0 && !gzeof(g));
    CHECK(gzread(g, back, 10) == -1 && gzgetc(g) == -1 && gzgets(g, (char *)back, 10) == NULL);
    CHECK(gzungetc('a', g) == -1 && gzrewind(g) == -1);
    CHECK(gzclose_r(g) == Z_STREAM_ERROR);
    CHECK(gzclose_w(g) == Z_OK);

    gzFile r = gzopen(f, "rb");
    CHECK(gzbuffer(r, 1000) == 0);
    CHECK(gzfread(back, 8, n / 8 + 4, r) == n / 8 && memcmp(back, data, n) == 0);
    CHECK(gzoffset(r) == file_size(f));
    CHECK(gzwrite(r, data, 10) == 0 && gzputc(r, 'a') == -1 && gzputs(r, "a") == -1);
    CHECK(gzprintf(r, "%d", 1) < 0 && gzflush(r, Z_SYNC_FLUSH) == Z_STREAM_ERROR);
    CHECK(gzsetparams(r, 1, Z_DEFAULT_STRATEGY) == Z_STREAM_ERROR);
    // A request that doesn't fit in an int, then gzclearerr
    CHECK(gzrewind(r) == 0);
    CHECK(gzread(r, back, (unsigned)INT_MAX + 1) == -1);
    gzerror(r, &err);
    CHECK(err == Z_STREAM_ERROR);
    CHECK(gzseek(r, 0, SEEK_SET) == -1);
    gzclearerr(r);
    gzerror(r, &err);
    CHECK(err == Z_OK && read_matches(r, 0, 100));
    CHECK(gzclose_w(r) == Z_STREAM_ERROR);
    CHECK(gzclose_r(r) == Z_OK);

    // "x" refuses an existing file, like zlib
    errno = 0;
    CHECK(gzopen(f, "wbx") == NULL && errno == EEXIST);

    // Levels and window sizes from the environment
    static const char *levels[] = {"19", "-5", "3"};
    for (int i = 0; i < 3; i++){
        setenv("ZSTD_LEVEL", levels[i], 1);
        CHECK(write_gz(f, "wb", data, n, 100000) == 0);
        CHECK(read_gz(f, back, n + 1, n + 1) == (long long)n && memcmp(back, data, n) == 0);
    }
    unsetenv("ZSTD_LEVEL");
    setenv("ZSTD_WINDOW_LOG", "10", 1);
    CHECK(write_gz(f, "wb", data, n, n) == 0);
    unsetenv("ZSTD_WINDOW_LOG");
    CHECK(read_gz(f, back, n + 1, 5000) == (long long)n && memcmp(back, data, n) == 0);
    CHECK(cli_matches(f, data, n));
    free(data);
    free(back);
}

// The handle table is still fixed at 64 (growable in Phase 4): writes past
// it fall back to gzip, and every file still reads back intact
static void test_many_handles(void){
    enum { FILES = 80, SIZE = 50000 };
    gzFile g[FILES];
    char name[64];
    unsigned char *data = make_data(SIZE * FILES);
    unsigned char back[SIZE + 1];

    for (int i = 0; i < FILES; i++){
        snprintf(name, sizeof(name), "z_many_%02d.gz", i);
        g[i] = gzopen(path(name), "wb");
    }
    int ok = 1;
    for (int off = 0; off < SIZE; off += 10000){
        for (int i = 0; i < FILES; i++)
            ok &= gzwrite(g[i], data + i * SIZE + off, 10000) == 10000;
    }
    for (int i = 0; i < FILES; i++){
        ok &= gzclose(g[i]) == Z_OK;
    }
    CHECK(ok);

    int zstd = 0;
    for (int i = 0; i < FILES; i++){
        snprintf(name, sizeof(name), "z_many_%02d.gz", i);
        zstd += magic(path(name)) == 'z';
        g[i] = gzopen(path(name), "rb");
    }
    printf("    %d of %d files zstd\n", zstd, FILES);
    CHECK(zstd == 64);
    ok = 1;
    for (int i = 0; i < FILES; i++){
        ok &= g[i] && gzread(g[i], back, sizeof(back)) == SIZE && memcmp(back, data + i * SIZE, SIZE) == 0;
        gzclose(g[i]);
    }
    CHECK(ok);
    free(data);
}


/* Main */

static void run(const char *name, void (*test)(void)){
    int before = failures;
    printf("%s\n", name);
    test();
    printf("  %s\n", failures == before ? "ok" : "FAILED");
}

int main(void){
    setvbuf(stdout, NULL, _IOLBF, 0);
    mkdir("test", 0755);
    mkdir(DIR, 0755);
    setenv("ZSTD_FILE_TARGETS", "z_*", 1);
    unsetenv("ZSTD_LEVEL");
    unsetenv("ZSTD_WINDOW_LOG");
    unsetenv("ZSTD_HEADER_CACHE");

    // The library's lines go to the log
    int log = open(LOG, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    if (log < 0 || dup2(log, STDERR_FILENO) < 0){
        perror(LOG);
        return 2;
    }
    close(log);

    const char *probe = path("z_probe.gz");
    unsigned char byte = 0;
    if (write_gz(probe, "wb", &byte, 1, 1) != 0 || magic(probe) != 'z'){
        printf("%s isn't zstd: run with LD_PRELOAD=$PWD/bin/zstd_lib.so\n", probe);
        return 2;
    }
    cli = system("LD_PRELOAD= zstd --version > /dev/null 2>&1") == 0;
    printf("zlib %s, zstd tool %s\n", zlibVersion(), cli ? "found" : "not found (skipping its checks)");

    run("one large write", test_large_write);
    run("small writes of odd sizes, tiny reads", test_small_writes);
    run("gzputs, gzprintf, gzputc, gzflush mid-stream", test_text_and_flush);
    run("gzgets, gzgetc, gzungetc", test_gets_getc_ungetc);
    run("seeks, gztell, header cache", test_seeks);
    run("concatenated frames", test_concatenated);
    run("truncated and damaged files", test_truncated);
    run("empty file", test_empty);
    run("file over 4 GB", test_over_4gb);
    run("gzip, plain and gzdopen files", test_gzip_and_raw);
    run("gzbuffer, gzoffset, gzfread/gzfwrite, misuse, levels", test_api);
    run("more than 64 open handles", test_many_handles);
    printf("fork with open handles\n  skipped (Phase 4)\n");

    printf("\n%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
