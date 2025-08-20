#! /bin/sh
sudo umount /mnt/sdcard-rv
rm sdcard-rv-onsite.img
cp sdcard-rv-onsite.img.bak sdcard-rv-onsite.img
sudo mount -o loop sdcard-rv-onsite.img /mnt/sdcard-rv
sudo mkdir /mnt/sdcard-rv/proj
sudo touch /mnt/sdcard-rv/proj/README.md

