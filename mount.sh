sudo mount -o loop sdcard-rv.img /mnt/sdcard-rv
sudo mount -o loop sdcard-la.img /mnt/sdcard-la
sudo mount -o loop sdcard-rv-final.img /mnt/sdcard-rv-final
sudo mount -o loop sdcard-la-final.img /mnt/sdcard-la-final
sudo ln -sf /mnt/sdcard-rv/musl/lib/libc.so /lib/ld-musl-riscv64.so.1