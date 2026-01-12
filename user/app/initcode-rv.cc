#include "user.hh"

extern "C"
{
    struct linux_dirent64 {
        uint64 d_ino;
        int64 d_off;
        unsigned short d_reclen;
        unsigned char d_type;
        char d_name[];
    };

    __attribute__((section(".text.startup"))) int main()
    {
        // printf("DEBUG: Creating test file /fat32/hello.txt\n");
        // // O_CREAT=0100, O_RDWR=02
        // int fd_creat = openat(AT_FDCWD, "/fat32/hello.txt", 0100 | 02);
        // if (fd_creat >= 0) {
        //     printf("DEBUG: Created /fat32/hello.txt, fd=%d\n", fd_creat);
        //     write(fd_creat, "Hello FAT32", 11);
        //     close(fd_creat);
        // } else {
        //     printf("DEBUG: Failed to create file: %d\n", fd_creat);
        // }

        // printf("DEBUG: Creating test dir /fat32/testdir\n");
        // int mkdir_ret = mkdir("/fat32/testdir", 0777);
        // printf("DEBUG: mkdir ret=%d\n", mkdir_ret);

        // printf("DEBUG: Listing /fat32 content:\n");
        // // O_RDONLY=0, O_DIRECTORY=0200000
        // int fd = openat(AT_FDCWD, "/fat32", 0 | 0200000); 
        // if (fd < 0) {
        //      printf("DEBUG: Failed to open /fat32 with O_DIRECTORY, trying without...\n");
        //      fd = openat(AT_FDCWD, "/fat32", 0);
        // }

        // if (fd < 0) {
        //      printf("DEBUG: Failed to open /fat32 (ret=%d)\n", fd);
        // } else {
        //      printf("DEBUG: Opened /fat32 (fd=%d)\n", fd);
        //      char buf[1024];
        //      while(1) {
        //          int n = getdents64(fd, (struct linux_dirent64*)buf, 1024);
        //          printf("DEBUG: getdents64 returned %d\n", n);
        //          if (n < 0) {
        //               printf("DEBUG: getdents64 failed\n");
        //               break;
        //          }
        //          if (n == 0) break;
        //          for (int i = 0; i < n; ) {
        //               struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + i);
        //               printf("DEBUG: Found file: %s\n", d->d_name);
        //               i += d->d_reclen;
        //          }
        //      }
        //      close(fd);
        // }
        chdir("/fat32");
        basic_glibc_test();
        shutdown();
        return 0;
    }
}