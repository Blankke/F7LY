# riscv/musl/busybox

测例数量：55

| 测例 | 状态 | 默认回归 | 命令 | 来源 | 备注 |
| --- | --- | --- | --- | --- | --- |
| [echo "#### independent command test"](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox echo "#### independent command test"` | disk-command-list | busybox_cmd.txt:1 |
| [ash -c exit](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox ash -c exit` | disk-command-list | busybox_cmd.txt:2 |
| [sh -c exit](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox sh -c exit` | disk-command-list | busybox_cmd.txt:3 |
| [basename /aaa/bbb](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox basename /aaa/bbb` | disk-command-list | busybox_cmd.txt:4 |
| [cal](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox cal` | disk-command-list | busybox_cmd.txt:5 |
| [clear](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox clear` | disk-command-list | busybox_cmd.txt:6 |
| [date](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox date` | disk-command-list | busybox_cmd.txt:7 |
| [df](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox df` | disk-command-list | busybox_cmd.txt:8 |
| [dirname /aaa/bbb](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox dirname /aaa/bbb` | disk-command-list | busybox_cmd.txt:9 |
| [dmesg](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox dmesg` | disk-command-list | busybox_cmd.txt:10 |
| [du](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox du` | disk-command-list | busybox_cmd.txt:11 |
| [expr 1 + 1](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox expr 1 + 1` | disk-command-list | busybox_cmd.txt:12 |
| [false](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox false` | disk-command-list | busybox_cmd.txt:13 |
| [true](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox true` | disk-command-list | busybox_cmd.txt:14 |
| [which ls](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox which ls` | disk-command-list | busybox_cmd.txt:15 |
| [uname](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox uname` | disk-command-list | busybox_cmd.txt:16 |
| [uptime](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox uptime` | disk-command-list | busybox_cmd.txt:17 |
| [printf "abc\\n"](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox printf "abc\\n"` | disk-command-list | busybox_cmd.txt:18 |
| [ps](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox ps` | disk-command-list | busybox_cmd.txt:19 |
| [pwd](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox pwd` | disk-command-list | busybox_cmd.txt:20 |
| [free](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox free` | disk-command-list | busybox_cmd.txt:21 |
| [hwclock](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox hwclock` | disk-command-list | busybox_cmd.txt:22 |
| [sh -c 'sleep 5' & ./busybox kill $!](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox sh -c 'sleep 5' & ./busybox kill $!` | disk-command-list | busybox_cmd.txt:23 |
| [ls](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox ls` | disk-command-list | busybox_cmd.txt:24 |
| [sleep 1](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox sleep 1` | disk-command-list | busybox_cmd.txt:25 |
| [echo "#### file opration test"](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox echo "#### file opration test"` | disk-command-list | busybox_cmd.txt:26 |
| [touch test.txt](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox touch test.txt` | disk-command-list | busybox_cmd.txt:27 |
| [echo "hello world" > test.txt](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox echo "hello world" > test.txt` | disk-command-list | busybox_cmd.txt:28 |
| [cat test.txt](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox cat test.txt` | disk-command-list | busybox_cmd.txt:29 |
| [cut -c 3 test.txt](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox cut -c 3 test.txt` | disk-command-list | busybox_cmd.txt:30 |
| [od test.txt](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox od test.txt` | disk-command-list | busybox_cmd.txt:31 |
| [head test.txt](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox head test.txt` | disk-command-list | busybox_cmd.txt:32 |
| [tail test.txt](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox tail test.txt` | disk-command-list | busybox_cmd.txt:33 |
| [hexdump -C test.txt](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox hexdump -C test.txt` | disk-command-list | busybox_cmd.txt:34 |
| [md5sum test.txt](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox md5sum test.txt` | disk-command-list | busybox_cmd.txt:35 |
| [echo "ccccccc" >> test.txt](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox echo "ccccccc" >> test.txt` | disk-command-list | busybox_cmd.txt:36 |
| [echo "bbbbbbb" >> test.txt](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox echo "bbbbbbb" >> test.txt` | disk-command-list | busybox_cmd.txt:37 |
| [echo "aaaaaaa" >> test.txt](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox echo "aaaaaaa" >> test.txt` | disk-command-list | busybox_cmd.txt:38 |
| [echo "2222222" >> test.txt](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox echo "2222222" >> test.txt` | disk-command-list | busybox_cmd.txt:39 |
| [echo "1111111" >> test.txt](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox echo "1111111" >> test.txt` | disk-command-list | busybox_cmd.txt:40 |
| [echo "bbbbbbb" >> test.txt](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox echo "bbbbbbb" >> test.txt` | disk-command-list | busybox_cmd.txt:41 |
| [sort test.txt \| ./busybox uniq](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox sort test.txt \| ./busybox uniq` | disk-command-list | busybox_cmd.txt:42 |
| [stat test.txt](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox stat test.txt` | disk-command-list | busybox_cmd.txt:43 |
| [strings test.txt](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox strings test.txt` | disk-command-list | busybox_cmd.txt:44 |
| [wc test.txt](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox wc test.txt` | disk-command-list | busybox_cmd.txt:45 |
| [[ -f test.txt ]](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox [ -f test.txt ]` | disk-command-list | busybox_cmd.txt:46 |
| [more test.txt](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox more test.txt` | disk-command-list | busybox_cmd.txt:47 |
| [rm test.txt](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox rm test.txt` | disk-command-list | busybox_cmd.txt:48 |
| [mkdir test_dir](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox mkdir test_dir` | disk-command-list | busybox_cmd.txt:49 |
| [mv test_dir test](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox mv test_dir test` | disk-command-list | busybox_cmd.txt:50 |
| [rmdir test](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox rmdir test` | disk-command-list | busybox_cmd.txt:51 |
| [grep hello busybox_cmd.txt](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox grep hello busybox_cmd.txt` | disk-command-list | busybox_cmd.txt:52 |
| [cp busybox_cmd.txt busybox_cmd.bak](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox cp busybox_cmd.txt busybox_cmd.bak` | disk-command-list | busybox_cmd.txt:53 |
| [rm busybox_cmd.bak](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox rm busybox_cmd.bak` | disk-command-list | busybox_cmd.txt:54 |
| [find -name "busybox_cmd.txt"](/mnt/sdcard-rv/musl/busybox_cmd.txt) |  |  | `./busybox find -name "busybox_cmd.txt"` | disk-command-list | busybox_cmd.txt:55 |
