/*
Read checks for compression_lib's gz read interception, following how FSL
(znzlib) and MRtrix read .nii.gz files. Passes with and without the library:

    ./bin/test_gz_read
    COMPRESSION_READS=1 LD_PRELOAD=$PWD/bin/compression_lib.so ./bin/test_gz_read
    COMPRESSION_READS=1 LD_PRELOAD=$PWD/bin/compression_lib.so ./bin/test_gz_read -big

-big also reads a ~5 GB file, whose size no longer fits the gzip trailer.
*/

#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <zlib.h>

#define TESTDIR "test/"
#define DATA_SIZE (8 << 20)
#define HEADER_SIZE 348
#define VOX_OFFSET 352

static unsigned char *expected;
static int failed = 0;

static void check(const char *name, int ok){
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    failed += !ok;
}

// Bytes that compress a little, like image data
static void fill(unsigned char *buf, size_t size, size_t start){
    for (size_t i = 0; i < size; i++){
        size_t j = start + i;
        // Smooth high bits, noisy low bits
        buf[i] = (unsigned char)(((j / 64) & 0xf0) | (((uint32_t)j * 2654435761u) >> 28));
    }
}

static int write_gz(const char *path, const char *mode, const void *buf, size_t size){
    gzFile gz = gzopen(path, mode);
    if (!gz){
        return -1;
    }
    size_t done = 0;
    while (done < size){
        unsigned n = size - done > (1u << 30) ? (1u << 30) : (unsigned)(size - done);
        if (gzwrite(gz, (const char *)buf + done, n) != (int)n){
            gzclose(gz);
            return -1;
        }
        done += n;
    }
    return gzclose(gz) == Z_OK ? 0 : -1;
}

// Read size bytes in chunks of at most chunk, as tools do
static size_t read_all(gzFile gz, unsigned char *buf, size_t size, size_t chunk){
    size_t done = 0;
    while (done < size){
        unsigned n = size - done > chunk ? (unsigned)chunk : (unsigned)(size - done);
        int got = gzread(gz, buf + done, n);
        if (got <= 0){
            break;
        }
        done += got;
    }
    return done;
}

static void test_header_probe(const char *path){
    // FSL's has_ascii_header: open, read 12 bytes, close
    unsigned char buf[12];
    gzFile gz = gzopen(path, "rb");
    int ok = gz && gzread(gz, buf, sizeof(buf)) == sizeof(buf) && memcmp(buf, expected, sizeof(buf)) == 0;
    if (gz) gzclose(gz);
    check("header probe (12 bytes)", ok);
}

static void test_fsl_style(const char *path){
    // Header, seek to vox_offset, data in chunks, then EOF
    unsigned char *buf = malloc(DATA_SIZE);
    gzFile gz = gzopen(path, "rb");
    int ok = gz && gzread(gz, buf, HEADER_SIZE) == HEADER_SIZE && memcmp(buf, expected, HEADER_SIZE) == 0;
    ok = ok && gzseek(gz, VOX_OFFSET, SEEK_SET) == VOX_OFFSET;
    size_t rest = DATA_SIZE - VOX_OFFSET;
    ok = ok && read_all(gz, buf, rest, 1 << 20) == rest && memcmp(buf, expected + VOX_OFFSET, rest) == 0;
    check("fsl style: header, seek, chunked data", ok);

    ok = gz && gztell(gz) == DATA_SIZE && !gzeof(gz);
    check("gztell at end, no eof before reading past it", ok);

    ok = gz && gzread(gz, buf, 10) == 0 && gzeof(gz);
    check("read past end returns 0 and sets eof", ok);

    // Back to the start after the whole file was read
    ok = gz && gzseek(gz, 100, SEEK_SET) == 100 && gzread(gz, buf, 100) == 100
        && memcmp(buf, expected + 100, 100) == 0 && !gzeof(gz);
    check("seek back after eof", ok);

    ok = gz && gzrewind(gz) == 0 && gzread(gz, buf, 12) == 12 && memcmp(buf, expected, 12) == 0;
    check("rewind", ok);

    ok = gz && gzseek(gz, DATA_SIZE - 10, SEEK_SET) == DATA_SIZE - 10 && gzread(gz, buf, 10) == 10
        && memcmp(buf, expected + DATA_SIZE - 10, 10) == 0;
    check("seek near the end after rewind", ok);

    if (gz) gzclose(gz);
    free(buf);
}

static void test_mrtrix_style(const char *path){
    // Header read in its own open, then one big read from vox_offset
    unsigned char *buf = malloc(DATA_SIZE);
    gzFile gz = gzopen(path, "rb");
    int ok = gz && gzread(gz, buf, HEADER_SIZE) == HEADER_SIZE;
    if (gz) gzclose(gz);

    gz = gzopen(path, "rb");
    size_t rest = DATA_SIZE - VOX_OFFSET;
    ok = ok && gz && gzseek(gz, VOX_OFFSET, SEEK_SET) == VOX_OFFSET
        && gzread(gz, buf, rest) == (int)rest && memcmp(buf, expected + VOX_OFFSET, rest) == 0;
    if (gz) gzclose(gz);
    check("mrtrix style: header open, then one big read", ok);
    free(buf);
}

static void test_seek_cur_and_fread(const char *path){
    unsigned char buf[4096];
    gzFile gz = gzopen(path, "rb");
    int ok = gz && gzseek(gz, 2 << 20, SEEK_CUR) == (2 << 20) && gztell(gz) == (2 << 20)
        && gzfread(buf, 16, 256, gz) == 256 && memcmp(buf, expected + (2 << 20), 4096) == 0;
    ok = ok && gzseek(gz, 0, SEEK_END) == -1;
    if (gz) gzclose(gz);
    check("SEEK_CUR, gzfread, SEEK_END refused", ok);
}

static void test_text(void){
    // Lines past the first MiB, read with gzgets/gzgetc/gzungetc
    const char *path = TESTDIR "read_text.gz";
    size_t lines = 300000;
    size_t size = 0;
    char *text = malloc(lines * 16);
    for (size_t i = 0; i < lines; i++){
        size += sprintf(text + size, "line %zu\n", i);
    }
    int ok = write_gz(path, "wb", text, size) == 0;

    gzFile gz = gzopen(path, "rb");
    char *skip = malloc(1500000);
    ok = ok && gz && gzread(gz, skip, 1500000) == 1500000;
    size_t pos = 1500000;

    char line[64];
    const char *nl = memchr(text + pos, '\n', size - pos);
    size_t len = nl - (text + pos) + 1;
    ok = ok && gzgets(gz, line, sizeof(line)) && strlen(line) == len && memcmp(line, text + pos, len) == 0;
    pos += len;

    ok = ok && gzgetc(gz) == (unsigned char)text[pos];
    ok = ok && gzungetc('X', gz) == 'X' && gzgetc(gz) == 'X';
    pos++;

    size_t count = 0;
    while (gzgets(gz, line, sizeof(line))){
        count++;
    }
    size_t expected_lines = 0;
    for (size_t i = pos; i < size; i++){
        expected_lines += text[i] == '\n';
    }
    ok = ok && count == expected_lines && gzeof(gz);
    if (gz) gzclose(gz);
    check("text: gzgets, gzgetc, gzungetc", ok);
    free(skip);
    free(text);
}

static void test_multi_member(void){
    // Two gzip members in one file; the library must leave this to zlib
    const char *path = TESTDIR "read_multi.gz";
    size_t half = DATA_SIZE / 2;
    int ok = write_gz(path, "wb", expected, half) == 0 && write_gz(path, "ab", expected + half, half) == 0;

    unsigned char *buf = malloc(DATA_SIZE);
    gzFile gz = gzopen(path, "rb");
    ok = ok && gz && read_all(gz, buf, DATA_SIZE, 3 << 20) == DATA_SIZE && memcmp(buf, expected, DATA_SIZE) == 0;
    if (gz) gzclose(gz);
    check("multi-member file", ok);
    free(buf);
}

static void test_plain(void){
    // gzopen reads uncompressed files as they are
    const char *path = TESTDIR "read_plain.nii";
    FILE *f = fopen(path, "wb");
    int ok = f && fwrite(expected, 1, DATA_SIZE, f) == DATA_SIZE;
    if (f) fclose(f);

    unsigned char *buf = malloc(DATA_SIZE);
    gzFile gz = gzopen(path, "rb");
    ok = ok && gz && gzdirect(gz) == 1;
    ok = ok && read_all(gz, buf, DATA_SIZE, 3 << 20) == DATA_SIZE && memcmp(buf, expected, DATA_SIZE) == 0;
    if (gz) gzclose(gz);
    check("plain (uncompressed) file", ok);
    free(buf);
}

static void test_big(void){
    // ~5 GB that compresses well: the gzip trailer only holds the size mod 2^32
    const char *path = TESTDIR "read_big.gz";
    size_t size = (5ul << 30) + 12345;
    size_t chunk = 256 << 20;
    unsigned char *buf = calloc(1, chunk);

    gzFile gz = gzopen(path, "wb1");
    int ok = gz != NULL;
    for (size_t done = 0; ok && done < size; done += chunk){
        size_t n = size - done < chunk ? size - done : chunk;
        memset(buf, 0, n);
        for (size_t i = 0; i < n; i += 4096) buf[i] = (unsigned char)((done + i) / 4096);
        ok = gzwrite(gz, buf, n) == (int)n;
    }
    if (gz) ok = gzclose(gz) == Z_OK && ok;

    gz = gzopen(path, "rb");
    ok = ok && gz;
    size_t done = 0;
    while (ok && done < size){
        int got = gzread(gz, buf, chunk);
        if (got <= 0) break;
        for (size_t i = 0; i < (size_t)got; i++){
            size_t j = done + i;
            unsigned char want = j % 4096 == 0 ? (unsigned char)(j / 4096) : 0;
            if (buf[i] != want){
                ok = 0;
                break;
            }
        }
        done += got;
    }
    ok = ok && done == size;
    if (gz) gzclose(gz);
    remove(path);
    check("big file (~5 GB, size wraps the gzip trailer)", ok);
    free(buf);
}

int main(int argc, char **argv){
    mkdir(TESTDIR, 0777);

    expected = malloc(DATA_SIZE);
    fill(expected, DATA_SIZE, 0);

    const char *path = TESTDIR "read_data.nii.gz";
    if (write_gz(path, "wb", expected, DATA_SIZE) != 0){
        perror("write " TESTDIR "read_data.nii.gz");
        return 1;
    }

    test_header_probe(path);
    test_fsl_style(path);
    test_mrtrix_style(path);
    test_seek_cur_and_fread(path);
    test_text();
    test_multi_member();
    test_plain();
    if (argc > 1 && strcmp(argv[1], "-big") == 0){
        test_big();
    }

    printf("%d failed\n", failed);
    free(expected);
    return failed ? 1 : 0;
}
