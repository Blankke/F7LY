# loongarch/glibc/iozone

测例数量：8

| 测例 | 状态 | 默认回归 | 命令 | 来源 | 备注 |
| --- | --- | --- | --- | --- | --- |
| [01-iozone](/mnt/sdcard-la/glibc/iozone_testcode.sh) |  |  | `./iozone -a -r 1k -s 4m` | disk-testcode-script |  |
| [02-iozone](/mnt/sdcard-la/glibc/iozone_testcode.sh) |  |  | `./iozone -t 4 -i 0 -i 1 -r 1k -s 1m` | disk-testcode-script |  |
| [03-iozone](/mnt/sdcard-la/glibc/iozone_testcode.sh) |  |  | `./iozone -t 4 -i 0 -i 2 -r 1k -s 1m` | disk-testcode-script |  |
| [04-iozone](/mnt/sdcard-la/glibc/iozone_testcode.sh) |  |  | `./iozone -t 4 -i 0 -i 3 -r 1k -s 1m` | disk-testcode-script |  |
| [05-iozone](/mnt/sdcard-la/glibc/iozone_testcode.sh) |  |  | `./iozone -t 4 -i 0 -i 5 -r 1k -s 1m` | disk-testcode-script |  |
| [06-iozone](/mnt/sdcard-la/glibc/iozone_testcode.sh) |  |  | `./iozone -t 4 -i 6 -i 7 -r 1k -s 1m` | disk-testcode-script |  |
| [07-iozone](/mnt/sdcard-la/glibc/iozone_testcode.sh) |  |  | `./iozone -t 4 -i 9 -i 10 -r 1k -s 1m` | disk-testcode-script |  |
| [08-iozone](/mnt/sdcard-la/glibc/iozone_testcode.sh) |  |  | `./iozone -t 4 -i 11 -i 12 -r 1k -s 1m` | disk-testcode-script |  |
