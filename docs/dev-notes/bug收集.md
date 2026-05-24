1. ioctl 没写对
2. glibc动态链接问题
3. sys_read 缓冲区问题？文件指针问题？(体现为musl busybox sh 未闭合的引号)
4. signal没写
5. ioctl不完善，可能有bug，而且有Unsupported ioctl command: 0x5413