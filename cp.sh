#! /bin/sh
sudo umount /mnt/sdcard-la
rm sdcard-la-onsite.img
cp sdcard-la-onsite.img.bak sdcard-la-onsite.img
sudo mount -o loop sdcard-la-onsite.img /mnt/sdcard-la
sudo mkdir /mnt/sdcard-la/proj
sudo touch /mnt/sdcard-la/proj/README.md

