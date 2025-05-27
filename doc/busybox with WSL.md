# 验证test-suit提供的busybox有ash
sudo mkdir /mnt/sdcard-rv
sudo mount -o loop sdcard-rv.img /mnt/sdcard-rv
挂载到自己的文件系统上

```bash
gkq@gkq:/mnt/sdcard-rv/glibc$ file busybox
busybox: ELF 64-bit LSB executable, UCB RISC-V, RVC, double-float ABI, version 1 (SYSV), statically linked, BuildID[sha1]=84bdbec99577a855e0295e4bef67adb826987d09, for GNU/Linux 4.15.0, stripped

gkq@gkq:/mnt/sdcard-rv/glibc$ sudo apt install qemu-user-static

gkq@gkq:/mnt/sdcard-rv/glibc$ qemu-riscv64-static busybox ash
```

后来发现是忘了
__attribute__(( section( ".user.init.data" ) )) const char	 ash[] = "ash";
bb_sh[0] = "ash"是没有用的





strace -e trace=process,file,desc sh -c "ls"