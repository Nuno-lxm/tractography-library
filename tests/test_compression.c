#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <zstd.h>
#include <stdlib.h>
#include <zlib.h>
#include <sys/stat.h>

#define TESTDIR "test/"


int write_raw(){
    int fd1 = open(TESTDIR "test1.txt", O_WRONLY | O_CREAT | O_TRUNC, 0664);
    if (fd1 < 0){
        perror("open " TESTDIR "test1.txt");
        return 1;
    }

    write(fd1, "Hello, World!", 13);
    close(fd1);
    return 0;
}

int write_zst(){
    char *compressed_data = "compressed data";
    size_t data_size = strlen(compressed_data);

    size_t bound = ZSTD_compressBound(data_size);
    void *compressed_buffer = malloc(bound);
    if (!compressed_buffer) {
        perror("malloc");
        return 1;
    }

    size_t compressed_size = ZSTD_compress(compressed_buffer, bound, compressed_data, data_size, 3);
    if (ZSTD_isError(compressed_size)) {
        perror("ZSTD_compress");
        free(compressed_buffer);
        return 1;
    }

    int fd2 = open(TESTDIR "test2.txt.zst", O_WRONLY | O_CREAT | O_TRUNC, 0664);
    if (fd2 < 0){
        perror("open " TESTDIR "test2.txt.zst");
        free(compressed_buffer);
        return 1;
    }

    write(fd2, compressed_buffer, compressed_size);
    close(fd2);
    free(compressed_buffer);
    return 0;
}

int write_gzip(){
    gzFile gz = gzopen(TESTDIR "test3.txt.gz", "wb");
    if (!gz) {
        perror("gzopen");
        return 1;
    }

    const char *data = "gzip compressed data";
    if (gzwrite(gz, data, strlen(data)) <= 0) {
        perror("gzwrite");
        gzclose(gz);
        return 1;
    }

    gzclose(gz);
    return 0;
}


int main(void) {
    mkdir(TESTDIR, 0777);

    if (write_raw() != 0) {
        return 1;
    }

    if (write_zst() != 0) {
        return 1;
    }

    if (write_gzip() != 0) {
        return 1;
    }
    
    return 0;
}