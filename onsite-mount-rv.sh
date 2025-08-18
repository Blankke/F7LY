#! /bin/sh
sudo umount /mnt/sdcard-rv
sudo mount -o loop sdcard-rv-onsite.img /mnt/sdcard-rv
sudo ln -sf /mnt/sdcard-rv/musl/lib/libc.so /lib/ld-musl-riscv64.so.1