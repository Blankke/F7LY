# loongarch/glibc/lmbench

测例数量：24

| 测例 | 状态 | 默认回归 | 命令 | 来源 | 备注 |
| --- | --- | --- | --- | --- | --- |
| [01-lmbench_all](/mnt/sdcard-la/glibc/lmbench_testcode.sh) |  |  | `./lmbench_all lat_syscall -P 1 null` | disk-testcode-script |  |
| [02-lmbench_all](/mnt/sdcard-la/glibc/lmbench_testcode.sh) |  |  | `./lmbench_all lat_syscall -P 1 read` | disk-testcode-script |  |
| [03-lmbench_all](/mnt/sdcard-la/glibc/lmbench_testcode.sh) |  |  | `./lmbench_all lat_syscall -P 1 write` | disk-testcode-script |  |
| [04-lmbench_all](/mnt/sdcard-la/glibc/lmbench_testcode.sh) |  |  | `./lmbench_all lat_syscall -P 1 stat /var/tmp/lmbench` | disk-testcode-script |  |
| [05-lmbench_all](/mnt/sdcard-la/glibc/lmbench_testcode.sh) |  |  | `./lmbench_all lat_syscall -P 1 fstat /var/tmp/lmbench` | disk-testcode-script |  |
| [06-lmbench_all](/mnt/sdcard-la/glibc/lmbench_testcode.sh) |  |  | `./lmbench_all lat_syscall -P 1 open /var/tmp/lmbench` | disk-testcode-script |  |
| [07-lmbench_all](/mnt/sdcard-la/glibc/lmbench_testcode.sh) |  |  | `./lmbench_all lat_select -n 100 -P 1 file` | disk-testcode-script |  |
| [08-lmbench_all](/mnt/sdcard-la/glibc/lmbench_testcode.sh) |  |  | `./lmbench_all lat_sig -P 1 install` | disk-testcode-script |  |
| [09-lmbench_all](/mnt/sdcard-la/glibc/lmbench_testcode.sh) |  |  | `./lmbench_all lat_sig -P 1 catch` | disk-testcode-script |  |
| [10-lmbench_all](/mnt/sdcard-la/glibc/lmbench_testcode.sh) |  |  | `./lmbench_all lat_sig -P 1 prot lat_sig` | disk-testcode-script |  |
| [11-lmbench_all](/mnt/sdcard-la/glibc/lmbench_testcode.sh) |  |  | `./lmbench_all lat_pipe -P 1` | disk-testcode-script |  |
| [12-lmbench_all](/mnt/sdcard-la/glibc/lmbench_testcode.sh) |  |  | `./lmbench_all lat_proc -P 1 fork` | disk-testcode-script |  |
| [13-lmbench_all](/mnt/sdcard-la/glibc/lmbench_testcode.sh) |  |  | `./lmbench_all lat_proc -P 1 exec` | disk-testcode-script |  |
| [14-lmbench_all](/mnt/sdcard-la/glibc/lmbench_testcode.sh) |  |  | `./lmbench_all lat_proc -P 1 shell` | disk-testcode-script |  |
| [15-lmbench_all](/mnt/sdcard-la/glibc/lmbench_testcode.sh) |  |  | `./lmbench_all lmdd label="File /var/tmp/XXX write bandwidth:" of=/var/tmp/XXX move=1m fsync=1 print=3` | disk-testcode-script |  |
| [16-lmbench_all](/mnt/sdcard-la/glibc/lmbench_testcode.sh) |  |  | `./lmbench_all lat_pagefault -P 1 /var/tmp/XXX` | disk-testcode-script |  |
| [17-lmbench_all](/mnt/sdcard-la/glibc/lmbench_testcode.sh) |  |  | `./lmbench_all lat_mmap -P 1 512k /var/tmp/XXX` | disk-testcode-script |  |
| [18-lmbench_all](/mnt/sdcard-la/glibc/lmbench_testcode.sh) |  |  | `./lmbench_all lat_fs /var/tmp` | disk-testcode-script |  |
| [19-lmbench_all](/mnt/sdcard-la/glibc/lmbench_testcode.sh) |  |  | `./lmbench_all bw_pipe -P 1` | disk-testcode-script |  |
| [20-lmbench_all](/mnt/sdcard-la/glibc/lmbench_testcode.sh) |  |  | `./lmbench_all bw_file_rd -P 1 512k io_only /var/tmp/XXX` | disk-testcode-script |  |
| [21-lmbench_all](/mnt/sdcard-la/glibc/lmbench_testcode.sh) |  |  | `./lmbench_all bw_file_rd -P 1 512k open2close /var/tmp/XXX` | disk-testcode-script |  |
| [22-lmbench_all](/mnt/sdcard-la/glibc/lmbench_testcode.sh) |  |  | `./lmbench_all bw_mmap_rd -P 1 512k mmap_only /var/tmp/XXX` | disk-testcode-script |  |
| [23-lmbench_all](/mnt/sdcard-la/glibc/lmbench_testcode.sh) |  |  | `./lmbench_all bw_mmap_rd -P 1 512k open2close /var/tmp/XXX` | disk-testcode-script |  |
| [24-lmbench_all](/mnt/sdcard-la/glibc/lmbench_testcode.sh) |  |  | `./lmbench_all lat_ctx -P 1 -s 32 2 4 8 16 24 32 64 96` | disk-testcode-script |  |
