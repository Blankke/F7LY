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

// getpriority/setpriority 的 which 参数
#define PRIO_PROCESS 0
#define PRIO_PGRP 1
#define PRIO_USER 2

// SysV SHM / IPC 常量，供内置研究程序与最小复现使用
#define IPC_CREAT 01000
#define IPC_EXCL 02000
#define IPC_PRIVATE 0
#define IPC_RMID 0

#define SHM_RDONLY 010000
#define SHM_RND 020000

// mmap/prot/map 标志，和内核实现保持一致
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4

#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS

#define MAP_FAILED ((void *)-1)

// 常用 open/lseek 标志，和内核/posix 语义保持一致
#define O_RDONLY 00
#define O_WRONLY 01
#define O_RDWR 02
#define O_CREAT 0100
#define O_TRUNC 01000

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

extern int errno;

struct user_timeval
{
    long tv_sec;
    long tv_usec;
};

int openat(int dirfd, const char *path, int flags);
int close(int fd);
ssize_t read(int fd, void *buf, size_t len);
ssize_t write(int fd, const void *buf, size_t len);
pid_t getpid(void);
pid_t getppid(void);
int sched_yield(void);
int setpriority(int which, int who, int prio);
int getpriority(int which, int who);
int shmget(int key, size_t size, int shmflg);
int shmctl(int shmid, int cmd, void *buf);
void *shmat(int shmid, const void *shmaddr, int shmflg);
int shmdt(const void *shmaddr);
pid_t clone(int (*fn)(void *arg), void *arg, void *stack, size_t stack_size, unsigned long flags);
void exit(int code);
int waitpid(int pid, int *code, int options);
int exec(char *name);
int execve(const char *name, char *const argv[], char *const argp[]);
int setpgid(pid_t pid, pid_t pgid);
clock_t times(void *mytimes);
int gettimeofday(struct user_timeval *tv, int tz);
void *mmap(void *start, size_t len, int prot, int flags, int fd, off_t off);
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
int libcbench_test(const char *path);
int iozone_test(const char *path);
int lmbench_test(const char *path);
int lua_test(const char *path);
int basic_test(const char *path);
int busybox_test(const char *path);
int libc_test(const char *path);
int libc_subset_test(const char *path, const char *const cases[]);
int ltp_test(bool is_musl);
int basic_subset_test(const char *path, const char *const cases[]);
int ltp_subset_test(bool is_musl, const char *const cases[]);
int priority_ltp_regression_riscv(void);
int regression_suite_4d1444(void);
int iozone_mclock_research(void);
int final_test_musl(void);
int final_test_glibc(void);
int git_test(const char *path);
int vim_h();
int gcc_test();
int rustc_test();
// init函数
void init_env(const char *path);
