#! /bin/sh
sudo mount -o loop sdcard-rv.img /mnt/sdcard-rv
sudo ln -sf /mnt/sdcard-rv/musl/lib/libc.so /lib/ld-musl-riscv64.so.1