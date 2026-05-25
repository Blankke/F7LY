# loongarch/musl/busybox

测例数量：55

| 测例 | 是否通过 | 备注 |
| --- | --- | --- |
| echo "#### independent command test" |  | busybox_cmd.txt:1 |
| ash -c exit |  | busybox_cmd.txt:2 |
| sh -c exit |  | busybox_cmd.txt:3 |
| basename /aaa/bbb |  | busybox_cmd.txt:4 |
| cal |  | busybox_cmd.txt:5 |
| clear |  | busybox_cmd.txt:6 |
| date |  | busybox_cmd.txt:7 |
| df |  | busybox_cmd.txt:8 |
| dirname /aaa/bbb |  | busybox_cmd.txt:9 |
| dmesg |  | busybox_cmd.txt:10 |
| du |  | busybox_cmd.txt:11 |
| expr 1 + 1 |  | busybox_cmd.txt:12 |
| false |  | busybox_cmd.txt:13 |
| true |  | busybox_cmd.txt:14 |
| which ls |  | busybox_cmd.txt:15 |
| uname |  | busybox_cmd.txt:16 |
| uptime |  | busybox_cmd.txt:17 |
| printf "abc\\n" |  | busybox_cmd.txt:18 |
| ps |  | busybox_cmd.txt:19 |
| pwd |  | busybox_cmd.txt:20 |
| free |  | busybox_cmd.txt:21 |
| hwclock |  | busybox_cmd.txt:22 |
| sh -c 'sleep 5' & ./busybox kill $! |  | busybox_cmd.txt:23 |
| ls |  | busybox_cmd.txt:24 |
| sleep 1 |  | busybox_cmd.txt:25 |
| echo "#### file opration test" |  | busybox_cmd.txt:26 |
| touch test.txt |  | busybox_cmd.txt:27 |
| echo "hello world" > test.txt |  | busybox_cmd.txt:28 |
| cat test.txt |  | busybox_cmd.txt:29 |
| cut -c 3 test.txt |  | busybox_cmd.txt:30 |
| od test.txt |  | busybox_cmd.txt:31 |
| head test.txt |  | busybox_cmd.txt:32 |
| tail test.txt |  | busybox_cmd.txt:33 |
| hexdump -C test.txt |  | busybox_cmd.txt:34 |
| md5sum test.txt |  | busybox_cmd.txt:35 |
| echo "ccccccc" >> test.txt |  | busybox_cmd.txt:36 |
| echo "bbbbbbb" >> test.txt |  | busybox_cmd.txt:41 |
| echo "aaaaaaa" >> test.txt |  | busybox_cmd.txt:38 |
| echo "2222222" >> test.txt |  | busybox_cmd.txt:39 |
| echo "1111111" >> test.txt |  | busybox_cmd.txt:40 |
| echo "bbbbbbb" >> test.txt |  | busybox_cmd.txt:41 |
| sort test.txt \| ./busybox uniq |  | busybox_cmd.txt:42 |
| stat test.txt |  | busybox_cmd.txt:43 |
| strings test.txt |  | busybox_cmd.txt:44 |
| wc test.txt |  | busybox_cmd.txt:45 |
| [ -f test.txt ] |  | busybox_cmd.txt:46 |
| more test.txt |  | busybox_cmd.txt:47 |
| rm test.txt |  | busybox_cmd.txt:48 |
| mkdir test_dir |  | busybox_cmd.txt:49 |
| mv test_dir test |  | busybox_cmd.txt:50 |
| rmdir test |  | busybox_cmd.txt:51 |
| grep hello busybox_cmd.txt |  | busybox_cmd.txt:52 |
| cp busybox_cmd.txt busybox_cmd.bak |  | busybox_cmd.txt:53 |
| rm busybox_cmd.bak |  | busybox_cmd.txt:54 |
| find -name "busybox_cmd.txt" |  | busybox_cmd.txt:55 |
