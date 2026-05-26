# riscv/glibc/busybox

测例数量：55

| 测例 | 是否通过 | 备注 |
| --- | --- | --- |
| echo "#### independent command test" | PASS | null|
| ash -c exit | PASS | null|
| sh -c exit | PASS | null|
| basename /aaa/bbb | PASS | null|
| cal | PASS | null|
| clear | PASS | null|
| date | PASS | null|
| df | PASS | null|
| dirname /aaa/bbb | PASS | null|
| dmesg | PASS | null|
| du | PASS | null|
| expr 1 + 1 | PASS | null|
| false | PASS | null|
| true | PASS | null|
| which ls | PASS | null|
| uname | PASS | null|
| uptime | PASS | null|
| printf "abc\\n" | PASS | null|
| ps | PASS | null|
| pwd | PASS | null|
| free | PASS | null|
| hwclock | PASS | null|
| sh -c 'sleep 5' & ./busybox kill $! | PASS | null|
| ls | PASS | null|
| sleep 1 | PASS | null|
| echo "#### file opration test" | PASS | null|
| touch test.txt | PASS | null|
| echo "hello world" > test.txt | PASS | null|
| cat test.txt | PASS | null|
| cut -c 3 test.txt | PASS | null|
| od test.txt | PASS | null|
| head test.txt | PASS | null|
| tail test.txt | PASS | null|
| hexdump -C test.txt | PASS | null|
| md5sum test.txt | PASS | null|
| echo "ccccccc" >> test.txt | PASS | null|
| echo "bbbbbbb" >> test.txt | PASS | null|
| echo "aaaaaaa" >> test.txt | PASS | null|
| echo "2222222" >> test.txt | PASS | null|
| echo "1111111" >> test.txt | PASS | null|
| echo "bbbbbbb" >> test.txt | PASS | null|
| sort test.txt \| ./busybox uniq | PASS | null|
| stat test.txt | PASS | null|
| strings test.txt | PASS | null|
| wc test.txt | PASS | null|
| [ -f test.txt ] | PASS | null|
| more test.txt | PASS | null|
| rm test.txt | PASS | null|
| mkdir test_dir | PASS | null|
| mv test_dir test | PASS | null|
| rmdir test | PASS | null|
| grep hello busybox_cmd.txt | PASS | null|
| cp busybox_cmd.txt busybox_cmd.bak | PASS | null|
| rm busybox_cmd.bak | PASS | null|
| find -name "busybox_cmd.txt" | PASS | null|
