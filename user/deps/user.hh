#pragma once

#include <stddef.h>
// #include <unistd.h>

#include <stdarg.h>
#include "types.hh"

// AT_* constants for *at system calls
#define AT_FDCWD -100
#define AT_REMOVEDIR 0x200
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_SYMLINK_FOLLOW 0x400
#define AT_EMPTY_PATH 0x1000

int openat(int dirfd, const char *path, int flags);
int close(int fd);
ssize_t read(int fd, void *buf, size_t len);
ssize_t write(int fd, const void *buf, size_t len);
pid_t getpid(void);
pid_t getppid(void);
int sched_yield(void);
pid_t clone(int (*fn)(void *arg), void *arg, void *stack, size_t stack_size, unsigned long flags);
void exit(int code);
int waitpid(int pid, int *code, int options);
int exec(char *name);
int execve(const char *name, char *const argv[], char *const argp[]);
clock_t times(void *mytimes);
int munmap(void *start, size_t len);
int wait(int *code);
int sys_linkat(int olddirfd, char *oldpath, int newdirfd, char *newpath, unsigned int flags);
int sys_unlinkat(int dirfd, char *path, unsigned int flags);
int unlink(char *path);
int uname(void *buf);
int brk(void *addr);
int sbrk(void *addr);
int chdir(const char *path);
int mkdir(const char *path, mode_t mode);
int getdents64(int fd, struct linux_dirent64 *dirp64, unsigned long len);
int pipe(int fd[2]);
int dup(int fd);
int mount(const char *special, const char *dir, const char *fstype, unsigned long flags, const void *data);
int umount(const char *special);
int fork(void);
char *getcwd(char *buf, size_t size);
int lseek(int fd, off_t offset, int whence);

// proc
int shutdown();

// add
int sleep(unsigned int seconds);

// sync functions
int fsync(int fd);
int fdatasync(int fd);

// debug
int userdebug1();
int userdebug2();
int userdebug3();
int userdebug4();



// 打印到指定文件描述符，支持%d, %x, %p, %s, %c, %%
void vprintf(int fd, const char *fmt, va_list ap);
void fprintf(int fd, const char *fmt, ...);
void printf(const char *fmt, ...);

// test函数
int run_test(const char *path, char *argv[] = 0, char *envp[] = 0);
int basic_musl_test(void);
int basic_glibc_test(void);
int busybox_musl_test(void);
int busybox_glibc_test(void);
int libc_musl_test(void);
int start_shell(void);
int libcbench_test(const char *path);
int iozone_test(const char *path);
int lmbench_test(const char *path);
int lua_test(const char *path);
int basic_test(const char *path);
int busybox_test(const char *path);
int libc_test(const char *path);
int ltp_test(bool is_musl);
int final_test_musl(void);
int final_test_glibc(void);
int git_test(const char *path);
int vim_h();
int gcc_test();

// init函数
void init_env(const char *path);