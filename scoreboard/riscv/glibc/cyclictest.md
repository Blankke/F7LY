# riscv/glibc/cyclictest

测例数量：5

| 测例 | 状态 | 默认回归 | 命令 | 来源 | 备注 |
| --- | --- | --- | --- | --- | --- |
| [01-cyclictest](/mnt/sdcard-rv/glibc/cyclictest_testcode.sh) |  |  | `./cyclictest $2` | disk-testcode-script |  |
| [02-run_cyclictest](/mnt/sdcard-rv/glibc/cyclictest_testcode.sh) |  |  | `run_cyclictest NO_STRESS_P1 "-a -i 1000 -t1  -p99 -D 1s -q"` | disk-testcode-script |  |
| [03-run_cyclictest](/mnt/sdcard-rv/glibc/cyclictest_testcode.sh) |  |  | `run_cyclictest NO_STRESS_P8 "-a -i 1000 -t8  -p99 -D 1s -q"` | disk-testcode-script |  |
| [04-run_cyclictest](/mnt/sdcard-rv/glibc/cyclictest_testcode.sh) |  |  | `run_cyclictest STRESS_P1 "-a -i 1000 -t1  -p99 -D 1s -q"` | disk-testcode-script |  |
| [05-run_cyclictest](/mnt/sdcard-rv/glibc/cyclictest_testcode.sh) |  |  | `run_cyclictest STRESS_P8 "-a -i 1000 -t8  -p99 -D 1s -q"` | disk-testcode-script |  |
