# loongarch/musl/netperf

测例数量：6

| 测例 | 状态 | 默认回归 | 命令 | 来源 | 备注 |
| --- | --- | --- | --- | --- | --- |
| [01-netperf](/mnt/sdcard-la/musl/netperf_testcode.sh) |  |  | `./netperf -H $ip -p $port -t $1 -l 1 -- $2` | disk-testcode-script |  |
| [02-run_netperf](/mnt/sdcard-la/musl/netperf_testcode.sh) |  |  | `run_netperf UDP_STREAM  "-s 16k -S 16k -m 1k -M 1k"` | disk-testcode-script |  |
| [03-run_netperf](/mnt/sdcard-la/musl/netperf_testcode.sh) |  |  | `run_netperf TCP_STREAM  "-s 16k -S 16k -m 1k -M 1k"` | disk-testcode-script |  |
| [04-run_netperf](/mnt/sdcard-la/musl/netperf_testcode.sh) |  |  | `run_netperf UDP_RR      "-s 16k -S 16k -m 1k -M 1k -r 64,64 -R 1"` | disk-testcode-script |  |
| [05-run_netperf](/mnt/sdcard-la/musl/netperf_testcode.sh) |  |  | `run_netperf TCP_RR      "-s 16k -S 16k -m 1k -M 1k -r 64,64 -R 1"` | disk-testcode-script |  |
| [06-run_netperf](/mnt/sdcard-la/musl/netperf_testcode.sh) |  |  | `run_netperf TCP_CRR     "-s 16k -S 16k -m 1k -M 1k -r 64,64 -R 1"` | disk-testcode-script |  |
