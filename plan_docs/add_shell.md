我想给我们的内核添加一个交互式shell。但是应该不用自己书写，只需要一个入口，按我的理解是做一个和我们目前initcode的位置同级的入口，在那里可以执行sdcard中有的busybox/ash，这个就可以作为交互式终端了。然后在makefile中加一个编译目标也就是这个入口，make shell r就是rv架构进入shell中，make shell l就是la架构进入shell中，make run等逻辑不变，不影响到内核其他功能。请你完成这个功能并添加验证通过，并在确认完成后删除你的验证调试代码

后面是我们遇到的问题
当前shell的问题在于似乎是stdin没有能够输入进去。在宿主机上执行make run shell后会进入一个界面。打开调试输出后有一个写输出
```sh
DEBUG:
cache 1: 0x00000000802b1150
cache 2: 0x00000000802b1158
cache 3: 0x00000000802b1160
cache 4: 0x00000000802b1168
cache 5: 0x00000000802b1170
sz: 2688
prog_pages: 1, total_pages: 2, total_size: 8192
initcode start: 0x0000000080278270, end: 0x0000000080278cf0
initcode size: 0x0000000000000a80, total allocated space: 0x0000000000002000
init_fs_table finished
Initializing virtual file system tree structure
[INFO] => register device loop-control to No.3
[INFO] => register device ramdisk to No.4
[INFO] => register device console to No.5
[cleanup_memory_manager] pcb=0x00000000802b21c0 pid=2 tid=2 mm=0x000000009f84de10 ref=1
[vmunmap] 触碰保留页 va=0x0000001ffffff000 req_start=0x0000001ffffff000 npages=0x0000000000000001 do_free=0 cur_pid=2 cur_tid=2 state=4 pt_base=0x00000000807ec000
[vmunmap] 触碰保留页 va=0x0000001fffffe000 req_start=0x0000001fffffe000 npages=0x0000000000000001 do_free=0 cur_pid=2 cur_tid=2 state=4 pt_base=0x00000000807ec000
[cleanup_memory_manager] free_all_memory done: pcb=0x00000000802b21c0 pid=2 tid=2 mm=0x000000009f84de10 ref=0
[cleanup_memory_manager] delete mm: pcb=0x00000000802b21c0 pid=2 tid=2 mm=0x000000009f84de10
[cleanup_memory_manager] delete mm complete: pcb=0x00000000802b21c0 pid=2 tid=2
[cleanup_memory_manager] pcb=0x00000000802b21c0 pid=2 tid=2 mm=0x000000009f857060 ref=1
[vmunmap] 触碰保留页 va=0x0000001ffffff000 req_start=0x0000001ffffff000 npages=0x0000000000000001 do_free=0 cur_pid=2 cur_tid=2 state=4 pt_base=0x00000000807f7000
[vmunmap] 触碰保留页 va=0x0000001fffffe000 req_start=0x0000001fffffe000 npages=0x0000000000000001 do_free=0 cur_pid=2 cur_tid=2 state=4 pt_base=0x00000000807f7000
[cleanup_memory_manager] free_all_memory done: pcb=0x00000000802b21c0 pid=2 tid=2 mm=0x000000009f857060 ref=0
[cleanup_memory_manager] delete mm: pcb=0x00000000802b21c0 pid=2 tid=2 mm=0x000000009f857060
[cleanup_memory_manager] delete mm complete: pcb=0x00000000802b21c0 pid=2 tid=2
#### F7LY INTERACTIVE SHELL START ####
[cleanup_memory_manager] pcb=0x00000000802b21c0 pid=3 tid=3 mm=0x000000009f84de10 ref=1
[vmunmap] 触碰保留页 va=0x0000001ffffff000 req_start=0x0000001ffffff000 npages=0x0000000000000001 do_free=0 cur_pid=3 cur_tid=3 state=4 pt_base=0x00000000807ec000
[vmunmap] 触碰保留页 va=0x0000001fffffe000 req_start=0x0000001fffffe000 npages=0x0000000000000001 do_free=0 cur_pid=3 cur_tid=3 state=4 pt_base=0x00000000807ec000
[cleanup_memory_manager] free_all_memory done: pcb=0x00000000802b21c0 pid=3 tid=3 mm=0x000000009f84de10 ref=0
[cleanup_memory_manager] delete mm: pcb=0x00000000802b21c0 pid=3 tid=3 mm=0x000000009f84de10
[cleanup_memory_manager] delete mm complete: pcb=0x00000000802b21c0 pid=3 tid=3
file_path: /etc/passwd
F7LY$ 



QEMU: Terminated
make[1]: Leaving directory '/home/czc/F7LY'
```
然后在F7LY$ 出现后输入就没有动静了，也没有新的syscall出现或报错，我认为是stdin根本没有进入/busybox/ash
就算打开最底层的日志也没有动静
```sh
[sigAction] Setting ignore handler for signal 22
[SyscallHandler::sys_setpgid] pid: 0, pgid: 3
[SyscallHandler::sys_setpgid] Successfully set pgid 3 for process 3
[SyscallHandler::sys_ioctl] fd: 10, cmd: 0x5410, arg: 0x00000000001858ac
[SyscallHandler::sys_ioctl] fd: 0, cmd: 0x5401, arg: 0x00000000001855b0
vfs_is_file_exist: file not found: /.ash_history
vfs_openat: file /.ash_history does not exist, flags: 32768
[open] failed for path: /.ash_history,err:-2
ProcessMemoryManager: heap grown successfully to 0x000000000018a000
[sys_brk][trace] proc=busybox pid=3 tid=3 req=0x000000000018a000 ret=0x000000000018a000 heap=[0x0000000000186000,0x000000000018a000)
[SyscallHandler::sys_ioctl] fd: 0, cmd: 0x5402, arg: 0x00000000001855f0
[open] using virtual file system for path: /etc/passwd
file_path: /etc/passwd
[SyscallHandler::sys_lseek] fd: 3, offset: -216, whence: 1
[SyscallHandler::sys_ioctl] fd: 0, cmd: 0x5413, arg: 0x00000000001854f8
[sigAction] Setting handler for signal 28: enter 0x00000000000fad84 flags: 0x0000000014000000 mask: 0x0000000000000000
[SyscallHandler::sys_ioctl] fd: 1, cmd: 0x5413, arg: 0x0000000000185488
F7LY$ ls

```
加上之前我认为的是输入没有做好，所以可以检查下输入的问题，看看是否是输入没有正确地传递到shell中，或者是shell没有正确地读取输入。可以通过在shell中添加一些调试输出，来确认输入是否被正确地接收和处理了。另外，也可以检查一下shell的初始化过程，看看是否有遗漏或者错误的地方，导致stdin没有被正确地设置。
# 最终目标是让make shell r和make shell l都能够进入到交互式shell中，并且能够正确地接收输入和输出结果。


# 目标更新：
当前shell能够接收输入，但是磁盘路径似乎有问题。
```sh
F7LY$ pwd
/proc
F7LY$ cd ..
F7LY$ ls
dev   etc   proc
F7LY$ pwd
/
F7LY$ history
   0 ls /fat32
   1 ls
   2 cd etc/
   3 ls
   4 cd ..
   5 cd proc/
   6 ls
   7 cat version 
   8 pwd
   9 cd ..
  10 ls
  11 pwd
  12 history
F7LY$ 
```
可以看到当前路径是/proc，ls命令只能看到dev、etc和proc三个目录，无法看到其他目录了。可以检查一下文件系统的挂载和路径解析的问题，我们挂载的文件系统应该包含了sdcard，然后当前这个shell应该能够访问到sdcard中的文件和目录。可以通过在shell中添加一些调试输出，来确认文件系统是否正确地挂载了，以及路径解析是否正确地工作了。另外，也可以检查一下shell的初始化过程，看看是否有遗漏或者错误的地方，导致文件系统没有被正确地挂载或者路径解析没有正确地设置。
另外，本来在initcode中会调用void init_env(const char *path = musl_dir)函数初始化bin文件夹，但是当前shell中不能直接调用，否则会报错找不到函数定义，这应该是构建的时候没有正确包含的问题，但是也不能直接执行这个函数后执行busybox ash，需要有参考逻辑写一个新的shell初始化函数。

目标
1、修复当前shell中路径问题，能够正确访问到sdcard中的文件和目录。
2、编写一个新的shell初始化函数，正确地初始化环境变量和文件系统。