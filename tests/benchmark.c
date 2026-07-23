#define _GNU_SOURCE
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>

#define TEST_DIR "tests/output/"
#define NS_PER_S 1000000000L

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / (double)NS_PER_S;
}

static void fill_buf(char *buf, size_t size) {
    for (size_t i = 0; i < size; i++)
        buf[i] = (char)(i & 0xFF);
}

static unsigned long sum_buf(const char *buf, size_t size) {
    unsigned long sum = 0;
    for (size_t i = 0; i < size; i++)
        sum += (unsigned char)buf[i];
    return sum;
}

static void report(const char *name, double elapsed, size_t bytes,
                   int write_calls, int read_calls) {
    double mb = bytes / (1024.0 * 1024.0);
    double mbps = mb / elapsed;
    printf("RESULT\t%s\t%.4f\t%.2f\t%.2f\t%d\t%d\n",
           name, elapsed, mb, mbps, write_calls, read_calls);
}

/*
 * B1: Write-heavy — sequential 8KB writes (like dwidenoise output)
 */
static int bench_write_heavy(size_t chunk, int count) {
    char *buf = malloc(chunk);
    if (!buf) { perror("malloc"); return -1; }
    fill_buf(buf, chunk);

    char path[256];
    snprintf(path, sizeof(path), TEST_DIR "write_heavy.nii.gz");

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) { perror("open"); free(buf); return -1; }

    double t0 = now();
    for (int i = 0; i < count; i++) {
        if (write(fd, buf, chunk) != (ssize_t)chunk) {
            perror("write"); close(fd); free(buf); return -1;
        }
    }
    double t1 = now();
    close(fd);
    unlink(path);

    report("write_heavy", t1 - t0, (size_t)count * chunk, count, 0);
    free(buf);
    return 0;
}

/*
 * B2: Read-heavy — sequential 8KB reads (like dwidenoise input)
 */
static int bench_read_heavy(size_t chunk, int count) {
    char *buf = malloc(chunk);
    if (!buf) { perror("malloc"); return -1; }

    char path[256];
    snprintf(path, sizeof(path), TEST_DIR "read_heavy.nii.gz");

    /* create the file to read */
    fill_buf(buf, chunk);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) { perror("open"); free(buf); return -1; }
    for (int i = 0; i < count; i++) {
        if (write(fd, buf, chunk) != (ssize_t)chunk) {
            perror("write"); close(fd); free(buf); return -1;
        }
    }
    close(fd);

    fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open"); free(buf); return -1; }

    unsigned long sum = 0;
    double t0 = now();
    for (int i = 0; i < count; i++) {
        ssize_t n = read(fd, buf, chunk);
        if (n != (ssize_t)chunk) {
            if (n < 0) perror("read");
            break;
        }
        sum += sum_buf(buf, (size_t)n);
    }
    double t1 = now();
    close(fd);
    unlink(path);

    report("read_heavy", t1 - t0, (size_t)count * chunk, 0, count);
    free(buf);
    return (int)(sum & 0xFF);
}

/*
 * B3: Pipeline — read input then write output (end-to-end dwidenoise)
 */
static int bench_pipeline(size_t chunk, int read_count, int write_count) {
    char *buf = malloc(chunk);
    if (!buf) { perror("malloc"); return -1; }

    char in_path[256], out_path[256];
    snprintf(in_path, sizeof(in_path), TEST_DIR "pipeline_in.nii.gz");
    snprintf(out_path, sizeof(out_path), TEST_DIR "pipeline_out.nii.gz");

    fill_buf(buf, chunk);
    int fd = open(in_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) { perror("open"); free(buf); return -1; }
    for (int i = 0; i < read_count; i++) {
        if (write(fd, buf, chunk) != (ssize_t)chunk) {
            perror("write"); close(fd); free(buf); return -1;
        }
    }
    close(fd);

    double t0 = now();

    fd = open(in_path, O_RDONLY);
    if (fd < 0) { perror("open"); free(buf); return -1; }
    for (int i = 0; i < read_count; i++) {
        ssize_t n = read(fd, buf, chunk);
        if (n != (ssize_t)chunk) { perror("read"); break; }
    }
    close(fd);

    fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) { perror("open"); free(buf); return -1; }
    for (int i = 0; i < write_count; i++) {
        if (write(fd, buf, chunk) != (ssize_t)chunk) {
            perror("write"); close(fd); free(buf); return -1;
        }
    }
    close(fd);

    double t1 = now();

    unlink(in_path);
    unlink(out_path);

    report("pipeline", t1 - t0,
           (size_t)(read_count + write_count) * chunk, write_count, read_count);
    free(buf);
    return 0;
}

/*
 * B4: Multi-file — concurrent writes to N files
 */
static int bench_multi_file(size_t chunk, int num_files, int chunks_per_file) {
    char *buf = malloc(chunk);
    if (!buf) { perror("malloc"); return -1; }
    fill_buf(buf, chunk);

    int *fds = malloc((size_t)num_files * sizeof(int));
    if (!fds) { free(buf); return -1; }

    char path[256];
    for (int i = 0; i < num_files; i++) {
        snprintf(path, sizeof(path), TEST_DIR "multi_%d.nii.gz", i);
        fds[i] = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fds[i] < 0) { perror("open"); free(buf); free(fds); return -1; }
    }

    double t0 = now();
    for (int j = 0; j < chunks_per_file; j++) {
        for (int i = 0; i < num_files; i++) {
            if (write(fds[i], buf, chunk) != (ssize_t)chunk) {
                perror("write"); free(buf); free(fds); return -1;
            }
        }
    }

    for (int i = 0; i < num_files; i++) {
        close(fds[i]);
        snprintf(path, sizeof(path), TEST_DIR "multi_%d.nii.gz", i);
        unlink(path);
    }
    double t1 = now();

    report("multi_file", t1 - t0,
           (size_t)num_files * (size_t)chunks_per_file * chunk,
           num_files * chunks_per_file, 0);
    free(buf);
    free(fds);
    return 0;
}

/*
 * B5: Non-matching — write to a file that does NOT match BUFFER_FILE_TARGETS
 * (uses .txt extension instead of .nii.gz)
 */
static int bench_non_matching(size_t chunk, int count) {
    char *buf = malloc(chunk);
    if (!buf) { perror("malloc"); return -1; }
    fill_buf(buf, chunk);

    char path[256];
    snprintf(path, sizeof(path), TEST_DIR "non_matching.txt");

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) { perror("open"); free(buf); return -1; }

    double t0 = now();
    for (int i = 0; i < count; i++) {
        if (write(fd, buf, chunk) != (ssize_t)chunk) {
            perror("write"); close(fd); free(buf); return -1;
        }
    }
    double t1 = now();
    close(fd);
    unlink(path);

    report("non_matching", t1 - t0, (size_t)count * chunk, count, 0);
    free(buf);
    return 0;
}

/*
 * B6: Size sweep — same total data at various chunk sizes
 */
static int bench_size_sweep(size_t total_bytes, int only_kb) {
    size_t sizes[] = {1024, 4096, 8192, 65536};
    int num_sizes = sizeof(sizes) / sizeof(sizes[0]);

    if (only_kb > 0) {
        size_t chunk = (size_t)only_kb * 1024;
        int count = (int)(total_bytes / chunk);
        if (count < 1) return 0;
        char *buf = malloc(chunk);
        if (!buf) { perror("malloc"); return -1; }
        fill_buf(buf, chunk);
        char path[256];
        snprintf(path, sizeof(path), TEST_DIR "sweep_%zu.nii.gz", chunk);
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd < 0) { perror("open"); free(buf); return -1; }
        double t0 = now();
        for (int i = 0; i < count; i++) {
            if (write(fd, buf, chunk) != (ssize_t)chunk) {
                perror("write"); close(fd); free(buf); return -1;
            }
        }
        double t1 = now();
        close(fd);
        unlink(path);
        char label[64];
        if (chunk < 1024)
            snprintf(label, sizeof(label), "sweep_%zuB", chunk);
        else if (chunk < 1024 * 1024)
            snprintf(label, sizeof(label), "sweep_%zuKB", chunk / 1024);
        else
            snprintf(label, sizeof(label), "sweep_%zuMB", chunk / (1024 * 1024));
        report(label, t1 - t0, (size_t)count * chunk, count, 0);
        free(buf);
        return 0;
    }

    char path[256];
    for (int s = 0; s < num_sizes; s++) {
        size_t chunk = sizes[s];
        int count = (int)(total_bytes / chunk);
        if (count < 1) continue;

        char *buf = malloc(chunk);
        if (!buf) { perror("malloc"); return -1; }
        fill_buf(buf, chunk);

        snprintf(path, sizeof(path), TEST_DIR "sweep_%zu.nii.gz", chunk);

        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd < 0) { perror("open"); free(buf); return -1; }

        double t0 = now();
        for (int i = 0; i < count; i++) {
            if (write(fd, buf, chunk) != (ssize_t)chunk) {
                perror("write"); close(fd); free(buf); return -1;
            }
        }
        double t1 = now();
        close(fd);
        unlink(path);

        char label[64];
        if (chunk < 1024)
            snprintf(label, sizeof(label), "sweep_%zuB", chunk);
        else if (chunk < 1024 * 1024)
            snprintf(label, sizeof(label), "sweep_%zuKB", chunk / 1024);
        else
            snprintf(label, sizeof(label), "sweep_%zuMB", chunk / (1024 * 1024));

        report(label, t1 - t0, (size_t)count * chunk, count, 0);
        free(buf);
    }
    return 0;
}

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s [test] [options]\n", prog);
    fprintf(stderr, "Tests:\n");
    fprintf(stderr, "  1  write_heavy   — sequential 8KB writes\n");
    fprintf(stderr, "  2  read_heavy    — sequential 8KB reads\n");
    fprintf(stderr, "  3  pipeline      — read then write\n");
    fprintf(stderr, "  4  multi_file    — concurrent writes to N files\n");
    fprintf(stderr, "  5  non_matching  — write to non-target .txt file\n");
    fprintf(stderr, "  6  size_sweep    — various chunk sizes (1KB–1MB)\n");
    fprintf(stderr, "  (default) all\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --write-mb  N    total MB for write tests (default 64)\n");
    fprintf(stderr, "  --read-mb   N    total MB for read tests (default 16)\n");
    fprintf(stderr, "  --chunk     N    chunk size in KB (default 8)\n");
    fprintf(stderr, "  --files     N    file count for multi_file (default 8)\n");
    fprintf(stderr, "  --sweep-size N  single chunk size in KB for sweep (default: all)\n");
}

int main(int argc, char **argv) {
    int test_all = 1;
    int test_num = 0;
    int write_mb = 64;
    int read_mb = 16;
    int chunk_kb = 8;
    int num_files = 8;
    int sweep_size_kb = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--write-mb") == 0 && i + 1 < argc)
            write_mb = atoi(argv[++i]);
        else if (strcmp(argv[i], "--read-mb") == 0 && i + 1 < argc)
            read_mb = atoi(argv[++i]);
        else if (strcmp(argv[i], "--chunk") == 0 && i + 1 < argc)
            chunk_kb = atoi(argv[++i]);
        else if (strcmp(argv[i], "--files") == 0 && i + 1 < argc)
            num_files = atoi(argv[++i]);
        else if (strcmp(argv[i], "--sweep-size") == 0 && i + 1 < argc)
            sweep_size_kb = atoi(argv[++i]);
        else if (argv[i][0] >= '1' && argv[i][0] <= '6' && argv[i][1] == '\0') {
            test_all = 0;
            test_num = argv[i][0] - '0';
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    size_t chunk = (size_t)chunk_kb * 1024;
    mkdir(TEST_DIR, 0777);

    printf("# test_name\ttime_s\tdata_mb\tmbps\twrite_calls\tread_calls\n");

    if (test_all || test_num == 1) {
        int count = write_mb * 1024 * 1024 / (int)chunk;
        if (bench_write_heavy(chunk, count) < 0) return 1;
    }
    if (test_all || test_num == 2) {
        int count = read_mb * 1024 * 1024 / (int)chunk;
        if (bench_read_heavy(chunk, count) < 0) return 1;
    }
    if (test_all || test_num == 3) {
        int rcount = read_mb * 1024 * 1024 / (int)chunk;
        int wcount = write_mb * 1024 * 1024 / (int)chunk;
        if (bench_pipeline(chunk, rcount, wcount) < 0) return 1;
    }
    if (test_all || test_num == 4) {
        int per_file = write_mb * 1024 * 1024 / (num_files * (int)chunk);
        if (per_file < 1) per_file = 1;
        if (bench_multi_file(chunk, num_files, per_file) < 0) return 1;
    }
    if (test_all || test_num == 5) {
        int count = write_mb * 1024 * 1024 / (int)chunk;
        if (bench_non_matching(chunk, count) < 0) return 1;
    }
    if (test_all || test_num == 6) {
        if (bench_size_sweep((size_t)write_mb * 1024 * 1024, sweep_size_kb) < 0) return 1;
    }

    return 0;
}
