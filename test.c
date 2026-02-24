#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

int main(void) {
    int fd1, fd2, fd3;
    char buffer[256];

    // Test 1: Multiple small writes to same fd (should buffer)
    printf("=== Test 1: Multiple small writes ===\n");
    fd1 = open("test1.txt", O_WRONLY | O_CREAT | O_TRUNC, 0664);
    
    printf("Writing AAA (3 bytes)\n");
    write(fd1, "AAA", 3);
    printf("Writing BBB (3 bytes)\n");
    write(fd1, "BBB", 3);
    printf("Writing CCC (3 bytes) - should trigger flush\n");
    write(fd1, "CCC", 3);
    printf("Writing DD (2 bytes)\n");
    write(fd1, "DD", 2);
    
    printf("Closing fd1 - should flush remaining bytes\n");
    close(fd1);

    // Test 2: Large write exceeding buffer
    printf("\n=== Test 2: Large write ===\n");
    fd2 = open("test2.txt", O_WRONLY | O_CREAT | O_TRUNC, 0664);
    if (fd2 < 0) {
        perror("open test2.txt");
        return 1;
    }
    write(fd2, "LARGE_WRITE_DATA", 16);
    close(fd2);

    // Test 3: Multiple file descriptors
    printf("\n=== Test 3: Multiple FDs ===\n");
    fd1 = open("test3a.txt", O_WRONLY | O_CREAT | O_TRUNC, 0664);
    fd2 = open("test3b.txt", O_WRONLY | O_CREAT | O_TRUNC, 0664);
    fd3 = open("test3c.txt", O_WRONLY | O_CREAT | O_TRUNC, 0664);

    if (fd1 < 0 || fd2 < 0 || fd3 < 0) {
        perror("open test3");
        return 1;
    }

    write(fd1, "File1:", 6);
    write(fd2, "File2:", 6);
    write(fd3, "File3:", 6);
    write(fd1, "AA", 2);
    write(fd2, "BB", 2);
    write(fd3, "CC", 2);
    write(fd1, "END", 3);
    write(fd2, "END", 3);
    write(fd3, "END", 3);

    close(fd1);
    close(fd2);
    close(fd3);

    // Verify write buffering results
    printf("\n=== Verifying Results ===\n");
    
    fd1 = open("test1.txt", O_RDONLY);
    ssize_t bytes = read(fd1, buffer, sizeof(buffer) - 1);
    buffer[bytes] = '\0';
    printf("test1.txt: %s\n", buffer);
    close(fd1);

    fd2 = open("test2.txt", O_RDONLY);
    bytes = read(fd2, buffer, sizeof(buffer) - 1);
    buffer[bytes] = '\0';
    printf("test2.txt: %s\n", buffer);
    close(fd2);

    fd1 = open("test3a.txt", O_RDONLY);
    bytes = read(fd1, buffer, sizeof(buffer) - 1);
    buffer[bytes] = '\0';
    printf("test3a.txt: %s\n", buffer);
    close(fd1);

    fd2 = open("test3b.txt", O_RDONLY);
    bytes = read(fd2, buffer, sizeof(buffer) - 1);
    buffer[bytes] = '\0';
    printf("test3b.txt: %s\n", buffer);
    close(fd2);

    fd3 = open("test3c.txt", O_RDONLY);
    bytes = read(fd3, buffer, sizeof(buffer) - 1);
    buffer[bytes] = '\0';
    printf("test3c.txt: %s\n", buffer);
    close(fd3);

    // Test 4: Prefetching behavior
    printf("\n=== Test 4: Prefetching ===\n");
    const char *prefetch_data =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    size_t prefetch_len = strlen(prefetch_data);

    int fdw = open("prefetch.txt", O_WRONLY | O_CREAT | O_TRUNC, 0664);
    if (fdw < 0) {
        perror("open prefetch.txt for write");
        return 1;
    }
    write(fdw, prefetch_data, prefetch_len);
    close(fdw);

    int fdr = open("prefetch.txt", O_RDONLY);
    if (fdr < 0) {
        perror("open prefetch.txt for read");
        return 1;
    }

    char chunk[6];              // read 5 bytes at a time
    char rebuilt[256];
    size_t rebuilt_len = 0;
    int api_reads = 0;

    while ((bytes = read(fdr, chunk, 5)) > 0) {
        memcpy(rebuilt + rebuilt_len, chunk, (size_t)bytes);
        rebuilt_len += (size_t)bytes;
        api_reads++;
    }
    close(fdr);

    rebuilt[rebuilt_len] = '\0';
    printf("prefetch.txt rebuilt ok: %s\n",
           (rebuilt_len == prefetch_len && strcmp(rebuilt, prefetch_data) == 0) ? "YES" : "NO");
    printf("API read() calls from app: %d\n", api_reads);

    return 0;
}