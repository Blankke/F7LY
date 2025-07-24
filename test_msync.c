#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#define TEST_FILE "test_msync_file.txt"
#define MAP_SIZE 4096

int main() {
    int fd;
    void *mapped_mem;
    char *test_data = "Hello, msync test!\n";
    
    printf("Testing msync system call implementation\n");
    
    // 创建测试文件
    fd = open(TEST_FILE, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd == -1) {
        perror("open");
        return 1;
    }
    
    // 扩展文件到一个页面大小
    if (ftruncate(fd, MAP_SIZE) == -1) {
        perror("ftruncate");
        close(fd);
        return 1;
    }
    
    // 写入初始数据
    if (write(fd, test_data, strlen(test_data)) == -1) {
        perror("write");
        close(fd);
        return 1;
    }
    
    // 内存映射文件
    mapped_mem = mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapped_mem == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }
    
    printf("File mapped at address: %p\n", mapped_mem);
    
    // 修改映射的内存
    strcpy((char*)mapped_mem, "Modified data for msync test!\n");
    printf("Modified mapped memory\n");
    
    // 测试 msync 异步同步
    printf("Testing msync with MS_ASYNC...\n");
    if (msync(mapped_mem, MAP_SIZE, MS_ASYNC) == -1) {
        perror("msync MS_ASYNC");
    } else {
        printf("msync MS_ASYNC succeeded\n");
    }
    
    // 测试 msync 同步同步
    printf("Testing msync with MS_SYNC...\n");
    if (msync(mapped_mem, MAP_SIZE, MS_SYNC) == -1) {
        perror("msync MS_SYNC");
    } else {
        printf("msync MS_SYNC succeeded\n");
    }
    
    // 测试 msync 带失效标志
    printf("Testing msync with MS_SYNC | MS_INVALIDATE...\n");
    if (msync(mapped_mem, MAP_SIZE, MS_SYNC | MS_INVALIDATE) == -1) {
        perror("msync MS_SYNC | MS_INVALIDATE");
    } else {
        printf("msync MS_SYNC | MS_INVALIDATE succeeded\n");
    }
    
    // 测试无效参数
    printf("Testing msync with invalid flags...\n");
    if (msync(mapped_mem, MAP_SIZE, MS_SYNC | MS_ASYNC) == -1) {
        printf("msync correctly rejected invalid flags: %s\n", strerror(errno));
    } else {
        printf("ERROR: msync should have rejected invalid flags\n");
    }
    
    // 测试未对齐地址
    printf("Testing msync with unaligned address...\n");
    if (msync((char*)mapped_mem + 1, MAP_SIZE - 1, MS_SYNC) == -1) {
        printf("msync correctly rejected unaligned address: %s\n", strerror(errno));
    } else {
        printf("ERROR: msync should have rejected unaligned address\n");
    }
    
    // 清理
    if (munmap(mapped_mem, MAP_SIZE) == -1) {
        perror("munmap");
    }
    
    close(fd);
    unlink(TEST_FILE);
    
    printf("msync test completed\n");
    return 0;
}
