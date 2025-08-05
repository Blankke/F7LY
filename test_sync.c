#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

int main() {
    const char* test_file = "/tmp/sync_test.txt";
    const char* test_data = "Hello, this is a test for fsync and fdatasync!\n";
    
    printf("Testing fsync and fdatasync system calls...\n");
    
    // Test 1: Test fsync with valid file descriptor
    printf("\n=== Test 1: fsync with valid file descriptor ===\n");
    int fd = open(test_file, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        perror("Failed to open test file");
        return 1;
    }
    
    ssize_t written = write(fd, test_data, strlen(test_data));
    if (written < 0) {
        perror("Failed to write to test file");
        close(fd);
        return 1;
    }
    printf("Written %zd bytes to %s\n", written, test_file);
    
    int result = fsync(fd);
    if (result == 0) {
        printf("fsync() succeeded for fd %d\n", fd);
    } else {
        printf("fsync() failed for fd %d: %s\n", fd, strerror(errno));
    }
    
    close(fd);
    
    // Test 2: Test fdatasync with valid file descriptor
    printf("\n=== Test 2: fdatasync with valid file descriptor ===\n");
    fd = open(test_file, O_WRONLY | O_APPEND);
    if (fd < 0) {
        perror("Failed to reopen test file");
        return 1;
    }
    
    const char* append_data = "Appended data for fdatasync test.\n";
    written = write(fd, append_data, strlen(append_data));
    if (written < 0) {
        perror("Failed to append to test file");
        close(fd);
        return 1;
    }
    printf("Appended %zd bytes to %s\n", written, test_file);
    
    result = fdatasync(fd);
    if (result == 0) {
        printf("fdatasync() succeeded for fd %d\n", fd);
    } else {
        printf("fdatasync() failed for fd %d: %s\n", fd, strerror(errno));
    }
    
    close(fd);
    
    // Test 3: Test fsync with invalid file descriptor
    printf("\n=== Test 3: fsync with invalid file descriptor ===\n");
    result = fsync(999);  // Invalid fd
    if (result == 0) {
        printf("fsync() unexpectedly succeeded with invalid fd\n");
    } else {
        printf("fsync() correctly failed with invalid fd: %s\n", strerror(errno));
    }
    
    // Test 4: Test fdatasync with invalid file descriptor
    printf("\n=== Test 4: fdatasync with invalid file descriptor ===\n");
    result = fdatasync(999);  // Invalid fd
    if (result == 0) {
        printf("fdatasync() unexpectedly succeeded with invalid fd\n");
    } else {
        printf("fdatasync() correctly failed with invalid fd: %s\n", strerror(errno));
    }
    
    // Test 5: Test fsync with pipe (should fail)
    printf("\n=== Test 5: fsync with pipe (should fail) ===\n");
    int pipefd[2];
    if (pipe(pipefd) == 0) {
        result = fsync(pipefd[1]);  // Try to sync write end of pipe
        if (result == 0) {
            printf("fsync() unexpectedly succeeded with pipe fd\n");
        } else {
            printf("fsync() correctly failed with pipe fd: %s\n", strerror(errno));
        }
        close(pipefd[0]);
        close(pipefd[1]);
    } else {
        printf("Failed to create pipe for test\n");
    }
    
    // Test 6: Test fsync with stdout (should work or be harmless)
    printf("\n=== Test 6: fsync with stdout ===\n");
    result = fsync(STDOUT_FILENO);
    if (result == 0) {
        printf("fsync() succeeded with stdout\n");
    } else {
        printf("fsync() failed with stdout: %s\n", strerror(errno));
    }
    
    printf("\nAll tests completed!\n");
    
    // Cleanup
    unlink(test_file);
    
    return 0;
}
