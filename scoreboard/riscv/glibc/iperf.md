# riscv/glibc/iperf

测例数量：8

| 测例 | 状态 | 默认回归 | 命令 | 来源 | 备注 |
| --- | --- | --- | --- | --- | --- |
| [01-$iperf](/mnt/sdcard-rv/glibc/iperf_testcode.sh) |  |  | `$iperf -c $host -p $port -t 2 -i 0 $args` | disk-testcode-script |  |
| [02-$iperf](/mnt/sdcard-rv/glibc/iperf_testcode.sh) |  |  | `$iperf -s -p $port -D` | disk-testcode-script |  |
| [03-run_iperf](/mnt/sdcard-rv/glibc/iperf_testcode.sh) |  |  | `run_iperf "BASIC_UDP" "-u -b 1000G"` | disk-testcode-script |  |
| [04-run_iperf](/mnt/sdcard-rv/glibc/iperf_testcode.sh) |  |  | `run_iperf "BASIC_TCP" ""` | disk-testcode-script |  |
| [05-run_iperf](/mnt/sdcard-rv/glibc/iperf_testcode.sh) |  |  | `run_iperf "PARALLEL_UDP" "-u -P 5 -b 1000G"` | disk-testcode-script |  |
| [06-run_iperf](/mnt/sdcard-rv/glibc/iperf_testcode.sh) |  |  | `run_iperf "PARALLEL_TCP" "-P 5"` | disk-testcode-script |  |
| [07-run_iperf](/mnt/sdcard-rv/glibc/iperf_testcode.sh) |  |  | `run_iperf "REVERSE_UDP" "-u -R -b 1000G"` | disk-testcode-script |  |
| [08-run_iperf](/mnt/sdcard-rv/glibc/iperf_testcode.sh) |  |  | `run_iperf "REVERSE_TCP" "-R"` | disk-testcode-script |  |
