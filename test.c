#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

int main(void) {
    int fd1, fd2, fd3;
    char buffer[256];

    // Test 1: Multiple small writes to same fd (should buffer)
    printf("=== Test 1: Multiple small writes ===\n");
    fd1 = open("test1.txt", O_WRONLY | O_CREAT | O_TRUNC, 0664);
    write(fd1, "AAA", 3);
    write(fd1, "BBB", 3);
    write(fd1, "CCC", 3);
    write(fd1, "DD", 2);
    close(fd1);

    // Test 2: Large write exceeding buffer
    printf("\n=== Test 2: Large write ===\n");
    fd2 = open("test2.txt", O_WRONLY | O_CREAT | O_TRUNC, 0664);
    write(fd2, "LARGE_WRITE_DATA", 16);
    close(fd2);

    // Test 3: Multiple file descriptors
    printf("\n=== Test 3: Multiple FDs ===\n");
    fd1 = open("test3a.txt", O_WRONLY | O_CREAT | O_TRUNC, 0664);
    fd2 = open("test3b.txt", O_WRONLY | O_CREAT | O_TRUNC, 0664);
    fd3 = open("test3c.txt", O_WRONLY | O_CREAT | O_TRUNC, 0664);
    write(fd1, "File1:", 6);
    write(fd2, "File2:", 6);
    write(fd3, "File3:", 6);
    write(fd1, "AA", 2);
    write(fd2, "BB", 2);
    write(fd3, "CC", 2);
    write(fd1, "END", 3);
    write(fd2, "END", 3);
    write(fd3, "END", 3);
    close(fd1); close(fd2); close(fd3);

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
    write(fdw, prefetch_data, prefetch_len);
    close(fdw);

    int fdr = open("prefetch.txt", O_RDONLY);
    char chunk[6];
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

    // Test 5: lseek() behavior
    printf("\n=== Test 5: lseek() flushes buffer ===\n");
    fd1 = open("seektest.txt", O_WRONLY | O_CREAT | O_TRUNC, 0664);
    write(fd1, "1234", 4); // Should be buffered
    lseek(fd1, 0, SEEK_SET); // Should flush buffer
    write(fd1, "ABCD", 4); // Should be buffered
    close(fd1);

    fd1 = open("seektest.txt", O_RDONLY);
    bytes = read(fd1, buffer, sizeof(buffer) - 1);
    buffer[bytes] = '\0';
    printf("seektest.txt: %s\n", buffer);
    close(fd1);

    // Test 6: fsync() and fdatasync()
    printf("\n=== Test 6: fsync/fdatasync flush ===\n");
    fd1 = open("synctest.txt", O_WRONLY | O_CREAT | O_TRUNC, 0664);
    write(fd1, "SYNC", 4); // Should be buffered
    fsync(fd1); // Should flush buffer
    write(fd1, "ED", 2); // Should be buffered
    fdatasync(fd1); // Should flush buffer
    close(fd1);

    fd1 = open("synctest.txt", O_RDONLY);
    bytes = read(fd1, buffer, sizeof(buffer) - 1);
    buffer[bytes] = '\0';
    printf("synctest.txt: %s\n", buffer);
    close(fd1);

    // Test 7: Write zero bytes
    printf("\n=== Test 7: Write zero bytes ===\n");
    fd1 = open("zerobyte.txt", O_WRONLY | O_CREAT | O_TRUNC, 0664);
    write(fd1, "", 0);
    close(fd1);

    fd1 = open("zerobyte.txt", O_RDONLY);
    bytes = read(fd1, buffer, sizeof(buffer) - 1);
    printf("zerobyte.txt bytes: %zd\n", bytes);
    close(fd1);

    // Test 8: File descriptor reuse
    printf("\n=== Test 8: File descriptor reuse ===\n");
    fd1 = open("reuse.txt", O_WRONLY | O_CREAT | O_TRUNC, 0664);
    write(fd1, "FIRST", 5);
    close(fd1);
    fd2 = open("reuse.txt", O_WRONLY | O_APPEND);
    write(fd2, "SECOND", 6);
    close(fd2);

    fd1 = open("reuse.txt", O_RDONLY);
    bytes = read(fd1, buffer, sizeof(buffer) - 1);
    buffer[bytes] = '\0';
    printf("reuse.txt: %s\n", buffer);
    close(fd1);

    // Test 9: Error handling (write to read-only file)
    printf("\n=== Test 9: Error handling ===\n");
    fd1 = open("readonly.txt", O_WRONLY | O_CREAT | O_TRUNC, 0444);
    close(fd1);
    fd1 = open("readonly.txt", O_RDONLY);
    ssize_t w = write(fd1, "FAIL", 4);
    printf("write() to read-only fd returned: %zd (errno=%d: %s)\n", w, errno, strerror(errno));
    close(fd1);

    // Test 10: Read zero bytes
    printf("\n=== Test 10: Read zero bytes ===\n");
    fd1 = open("test1.txt", O_RDONLY);
    bytes = read(fd1, buffer, 0);
    printf("read() zero bytes returned: %zd\n", bytes);
    close(fd1);

    // Test 11: Write/read after close (should fail)
    printf("\n=== Test 11: Write/read after close ===\n");
    fd1 = open("test1.txt", O_RDONLY);
    close(fd1);
    bytes = read(fd1, buffer, 5);
    printf("read() after close returned: %zd (errno=%d: %s)\n", bytes, errno, strerror(errno));
    fd1 = open("test1.txt", O_WRONLY);
    close(fd1);
    bytes = write(fd1, "X", 1);
    printf("write() after close returned: %zd (errno=%d: %s)\n", bytes, errno, strerror(errno));

    return 0;
}