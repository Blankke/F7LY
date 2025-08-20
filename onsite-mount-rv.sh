#! /bin/sh
sudo umount /mnt/sdcard-rv
sudo mount -o loop sdcard-rv-onsite.img /mnt/sdcard-rv
sudo mkdir /mnt/sdcard-rv/proj
sudo touch /mnt/sdcard-rv/proj/README.md