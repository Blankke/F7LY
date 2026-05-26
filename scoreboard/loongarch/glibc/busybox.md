# loongarch/glibc/busybox

测例数量：55

| 测例 | 是否通过 | 备注 |
| --- | --- | --- |
| echo "#### independent command test" | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| ash -c exit | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| sh -c exit | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| basename /aaa/bbb | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| cal | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| clear | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| date | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| df | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| dirname /aaa/bbb | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| dmesg | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| du | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| expr 1 + 1 | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| false | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| true | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| which ls | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| uname | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| uptime | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| printf "abc\\n" | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| ps | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| pwd | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| free | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| hwclock | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| sh -c 'sleep 5' & ./busybox kill $! | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| ls | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| sleep 1 | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| echo "#### file opration test" | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| touch test.txt | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| echo "hello world" > test.txt | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| cat test.txt | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| cut -c 3 test.txt | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| od test.txt | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| head test.txt | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| tail test.txt | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| hexdump -C test.txt | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| md5sum test.txt | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| echo "ccccccc" >> test.txt | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| echo "bbbbbbb" >> test.txt | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| echo "aaaaaaa" >> test.txt | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| echo "2222222" >> test.txt | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| echo "1111111" >> test.txt | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| echo "bbbbbbb" >> test.txt | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| sort test.txt \| ./busybox uniq | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| stat test.txt | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| strings test.txt | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| wc test.txt | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| [ -f test.txt ] | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| more test.txt | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| rm test.txt | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| mkdir test_dir | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| mv test_dir test | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| rmdir test | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| grep hello busybox_cmd.txt | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| cp busybox_cmd.txt busybox_cmd.bak | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| rm busybox_cmd.bak | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
| find -name "busybox_cmd.txt" | PASS | 2026-05-26 output_l_20260526-154450_full-regression_after-ltp-combo-table_snapshot_timeout-40m.txt |
