#pragma once

// // File access modes
// #define O_RDONLY  0x000
// #define O_WRONLY  0x001
// #define O_RDWR    0x002

// // File creation flags
// #define O_CREAT   0x100
// #define O_EXCL    0x200
// #define O_NOCTTY  0x400
// #define O_TRUNC   0x800

// // File status flags (可以被 fcntl F_SETFL 修改)
// #define O_APPEND  0x1000
// #define O_NONBLOCK 0x2000
// #define O_ASYNC   0x4000
// #define O_DIRECT  0x8000
// #define O_NOATIME 0x10000
// #define O_DSYNC   0x20000
// #define O_SYNC    0x40000

// // Other flags
// #define O_DIRECTORY 0x004
// #define O_CLOEXEC 0x008

// // fcntl commands
// #define F_DUPFD    0
// #define F_GETFD    1
// #define F_SETFD    2
// #define F_GETFL    3
// #define F_SETFL    4
// #define F_GETLK    5
// #define F_SETLK    6
// #define F_SETLKW   7
// #define F_SETOWN   8
// #define F_GETOWN   9
// #define F_SETSIG   10
// #define F_GETSIG   11
// #define F_SETLEASE 12
// #define F_GETLEASE 13
// #define F_NOTIFY   14
// #define F_SETPIPE_SZ 15
// #define F_GETPIPE_SZ 16
// #define F_ADD_SEALS 17
// #define F_GET_SEALS 18
// #define F_OFD_GETLK 19
// #define F_OFD_SETLK 20
// #define F_OFD_SETLKW 21
// #define F_DUPFD_CLOEXEC 1030

// // File descriptor flags
// #define FD_CLOEXEC 1

// #define AT_FDCWD -100
// #define AT_REMOVEDIR 0x200
// #define AT_SYMLINK_NOFOLLOW 0x100 /* Do not follow symbolic links.  */