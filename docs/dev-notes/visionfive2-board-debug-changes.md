# VisionFive2 上板调试修改整理

## 1. 对比基线与文档范围

- 对比基线：`33f3650826a5310bd7abe4fb15867ea94c61f4b3`（提交说明：`编译目标`）。
- 整理时间：2026-07-28。
- 本文记录当前工作区相对上述提交的实际代码差异、实机验证结果和待处理事项。
- 当前改动尚未提交；本文不代表最终实现已经完成验收。

`git diff --stat` 显示 23 个文件存在文本内容变化，共约 732 行新增、214 行删除。`git status` 还将
少量文件标记为修改但 `git diff` 没有显示文本差异，这类变化暂不计入功能改动，可能只是行尾格式。

## 2. 当前上板启动链路

```text
WSL 执行 make vf2 或 make vf2 shell
  -> 生成对应的 VisionFive2 内核二进制
  -> 拷贝到 SD 卡 FAT32 第 1 分区并重命名为 kernel
  -> U-Boot: load mmc 1:1 0x40200000 kernel
  -> U-Boot: go 0x40200000
  -> 内核初始化 VisionFive2 SD 控制器
  -> 读取 SD 卡 MBR
  -> 选择第一个 type=0x83 的 Linux 分区
  -> 当前实机选择 mmc1:2，起始 LBA=1050624
  -> 将该分区作为 ext4 根文件系统挂载到 /
  -> 初始化 /dev/misc、/dev/shm、/tmp、/usr、/usr/lib
  -> 进入用户态 initcode
```

当前 SD 卡分区布局：

```text
第 1 分区：FAT32，type=0x0c，存放 kernel 和 vf2_uEnv.txt
第 2 分区：Linux/ext4，type=0x83，存放 rootfs
```

## 3. 早期启动与串口定位

涉及文件：

- `kernel/boot/riscv/entry.S`
- `kernel/boot/riscv/start.cc`
- `kernel/boot/riscv/main.cc`
- `kernel/libs/printer.cc`
- `kernel/proc/proc_manager.cc`

主要修改：

1. 在 VF2 最早期启动路径中通过 SBI legacy console 增加阶段标记：
   `E0`、`E1`、`S0`、`S1`、`M0`、`P0`、`P1`。
2. 打印 `E0/E1` 时保存并恢复 U-Boot/OpenSBI 传入的 `a0/a1`，避免 SBI `ecall` 破坏 hartid 和 DTB 参数。
3. VF2 的 `Printer::init()` 不再重复调用 `kConsole.init()`，直接启用 printf、warn 和 info 输出组。
4. 在 `main()` 中增加磁盘、文件表、buffer cache、inode、VFS 等初始化阶段日志。
5. 在第一次 `fork_ret()` 中增加 `filesystem_init()` 前后日志，用于区分内核初始化和进程上下文中的文件系统初始化。

这些改动用于定位此前 `go 0x40200000` 后无输出的问题。实机已经能够依次打印早期标记、F7LY
横幅和完整内核初始化日志，说明从 U-Boot 跳转到 C++ 主初始化路径已经打通。

## 4. VF2 SD 分区解析与块设备映射

涉及文件：

- `kernel/fs/vfs/fs.cc`
- `kernel/fs/drivers/riscv/disk.cc`
- `kernel/fs/drivers/riscv/disk.hh`
- `kernel/fs/bio.cc`

主要修改：

1. `filesystem_init()` 在 VF2 上读取 SD 卡 LBA 0 的 MBR，并校验 `0x55aa` 签名。
2. 解析 4 个 DOS/MBR 分区表项，打印分区类型、起始 LBA 和扇区数量。
3. 选择第一个非空的 `type=0x83` Linux 分区作为 ext4 rootfs。
4. 新增 `disk_set_partition_offset()`，为内部逻辑设备保存分区起始扇区。
5. `disk_rw_sectors()` 在 VF2 上将文件系统逻辑扇区加上分区偏移，再调用 `sd_read()` 或 `sd_write()`。
6. `bio.cc` 的 VF2 读写改为经过 `disk_rw_sectors()`，避免绕过分区偏移直接访问整张 SD 卡。
7. 写路径增加逻辑扇区、物理扇区、分区偏移和扇区数量日志。

该修改解决了最初的错误：

```text
panic: filesystem_init: no bootable EXT4 root filesystem
```

实机确认当前选择结果：

```text
[fs] VF2 MBR part2 type=0x83 start=1050624 sectors=60389376
[fs] VF2 root uses mmc1:2, internal dev=1, start=1050624 sectors=60389376
[fs] probe dev 1 -> EXT4
```

## 5. SD 命令与读取路径增强

涉及文件：`kernel/fs/drivers/riscv/sdcard.cc`。

主要修改：

1. 汇总命令错误位和数据错误位：`EBE/SBE/HLE/RTO/RCRC/RESP_ERR` 以及
   `FRUN/HTO/DTO/DCRC`。
2. 新增 FIFO reset 和清除 `RINTSTS` 的辅助逻辑。
3. 命令发送前清状态；数据命令发送前复位 FIFO 并配置中断掩码。
4. 等待 `CMD_DONE` 时增加超时、寄存器快照和错误返回，不再无限轮询。
5. `sd_read()` 检查 `CMD17` 返回值；等待 FIFO 数据时加入超时和完整错误位检查。
6. 读取失败时打印扇区、剩余 word、`RINTSTS` 和 `STATUS`，必要时复位 FIFO。

实机已经能够稳定读取 MBR、ext4 超级块、inode 和目录，说明当前 SD 读取主路径基本打通。

## 6. SD 写路径的定位与正式化

涉及文件：

- `kernel/fs/drivers/riscv/sdcard.cc`
- `kernel/fs/drivers/riscv/disk.cc`
- `kernel/fs/lwext4/ext4_fs.cc`

原 `sd_write()` 的主要问题：

1. 使用 `while (RINTSTS & TXDR)` 决定整个扇区是否继续写，没有限制每次 `CMD24` 只能提交
   512 字节，即 128 个 `uint32`。
2. `TXDR` 是需要服务且可能保持置位的原始中断标志，不是当前扇区剩余数据量。写满 128 个 word
   后 `TXDR` 仍可能为 1，旧代码会继续向 FIFO 写入多余数据。
3. 如果 `TXDR` 尚未出现，旧循环会直接跳过，随后仍返回成功。
4. 旧代码忽略 `CMD24` 返回值，也不等待 `DATA_OVER`，无法确认写操作是否真正完成。
5. 多扇区写入时每轮都将 `tt` 重置为 0，导致后续扇区重复使用缓冲区开头的数据。
6. 没有统一处理命令超时、数据超时、CRC 和 FIFO 溢出/下溢错误。

第一轮诊断版完成了以下定位：

1. 校验缓冲区、大小和扇区参数。
2. 打印 `CMD24` 前后、等待 `TXDR`、FIFO 填充、等待 `DATA_OVER` 和最终结果。
3. 每个扇区使用 `source_base = i * 128` 和 `words_written` 选择正确的数据范围。
4. 使用 128 word 的安全上限，避免单个 `CMD24` 写入超过 512 字节。
5. 写完后等待 `DATA_OVER`，遇到超时或错误位则返回失败。
6. 恢复 ext4 可写挂载，使 ext4 自己的超级块写入触发真实 SD 写路径；没有增加额外测试扇区写入。

实机完成了一次 4096 字节、8 扇区的 ext4 写请求：

```text
[disk] write dev=1 logical=0 physical=1050624 cnt=8 offset=1050624
[sd_write] begin addr=1050624 size_words=1024 blocks=8
...
[sd_write] done addr=1050624 size_words=1024 blocks=8
[ext4_fs_init] after ext4_sb_write state r=0
```

每个扇区均观察到：

```text
CMD24 ret=0
words=128
RINTSTS=0x18（TXDR | DATA_OVER）
没有数据错误位
```

因此确认：VF2 SD 硬件写通路可以工作，ext4 可写挂载已经实机打通；旧代码卡住的主要原因是
FIFO 写循环没有按单扇区 128 word 明确结束。

在此基础上，当前代码已经进一步正式化：

1. 以 `words_written < 128` 作为单扇区写入主循环的不变量。
2. 检查 `FIFO_FULL`，FIFO 满时等待空间，不再依赖空延时循环。
3. 写满 128 word 后清理 TXDR，等待 `DATA_OVER`，并等待卡退出 busy。
4. 统一命令、FIFO、DATA_OVER 和 busy 超时后的 FIFO reset 与状态清理。
5. 删除 `wait_for_sdio_irq()`、`SD_Send_Command()` 中 `return` 后方的不可达旧实现。
6. VF2 首次真实写请求会逐扇区读取刚写入的相同位置并比较；该验证不会写额外测试扇区。

首次写后读回属于临时验收钩子。实机确认全部扇区出现 `readback verified` 后，可以关闭该钩子，
避免后续每次启动都为第一笔写请求增加额外读取。

## 7. ext4 与 VFS 调试改动

涉及文件：

- `kernel/fs/lwext4/ext4.cc`
- `kernel/fs/lwext4/ext4_fs.cc`
- `kernel/fs/lwext4/ext4_super.cc`
- `kernel/fs/vfs/vfs_ext4_ext.cc`
- `kernel/fs/vfs/fs.cc`

主要修改：

1. 为 `ext4_mount()`、`ext4_block_init()`、`ext4_fs_init()`、bcache 初始化与绑定增加阶段日志。
2. 为超级块读取、校验、feature 检查和超级块写回增加前后日志。
3. 移除整份 ext4 超级块 hexdump，改为打印 magic、inode 数、block 数和 block size 等摘要。
4. 为 `vfs_ext_namei()` 和 `/musl` 的 `ext4_raw_inode_fill()` 增加路径查找日志。
5. 为 `dir_init()` 中 `/dev/misc`、`/dev/shm`、`/tmp`、`/usr`、`/usr/lib` 的查找、创建和清理增加日志。
6. `filesystem_init()` 在挂载和 `dir_init()` 前后增加阶段日志。
7. `vfs_ext_mount2()` 的 VF2 分支当前仍显式以只读方式挂载 `root_fs`；本次实际根挂载走的是另一条
   `virtio_disk` 注册路径，并已通过 `ro=0` 完成可写挂载。

当前实机已经通过：

```text
[fs] 根文件系统已挂载到 / (dev=1, type=EXT4)
[fs] dir_init done
[fs] after dir_init
[fork_ret] filesystem_init done
```

`/dev/misc` 和 `/dev/shm` 不存在时也能够执行创建流程。

## 8. BusyBox shell、串口输入与网络隔离

涉及文件：

- `user/app/shell.cc`
- `kernel/devs/uart.cc`
- `kernel/devs/uart.hh`
- `kernel/proc/proc_manager.cc`
- `kernel/sys/syscall_handler.cc`
- `kernel/fs/vfs/file/device_file.cc`

主要修改：

1. `make vf2 shell` 使用交互式用户态入口，启动前只检查 `/bin/busybox`，不再依赖评测镜像中的
   `/musl`、`/glibc` 和 `/fat32`。
2. 排障阶段曾临时取消 `chdir("/root")`，让 shell 从 `/` 启动，以便先验证 BusyBox 和串口交互链路。
   在 `chdir()` 改用 `vfs_path_stat_noflush()`、VF2 网络残留线程被隔离后，当前代码已恢复从 `/root`
   启动。启动进程只切换一次目录，BusyBox 子进程直接继承；如果 `/root` 切换失败，会打印返回值并
   回退到 `/`。`PWD`、`OLDPWD` 会按实际启动目录设置，`HOME` 保持为 `/root`。
3. shell 启动阶段移除重复的用户态 F7LY 大横幅，避免用户态 `printf()` 将 Unicode 横幅拆成大量
   单字节 `write()`，并减少串口输出对调度时序的干扰。
4. VF2 的 `UartManager::read_ready()` 改用非阻塞 `sbi_console_getchar()` 探测输入。由于 SBI 调用会
   消费字符，探测到的字符先写入 UART 本地输入缓冲，后续 `read()` 再消费，避免 `ppoll()` 探测时丢键。
5. `get_char()` 和 `get_char_sync()` 优先读取上述缓冲；QEMU 仍使用原有 MMIO UART 就绪检测。
6. VF2 上暂时跳过 VirtIO 网络初始化。原因不是网络报错本身，而是 ONPS 在 VirtIO 网卡初始化失败后
   只退出 one-shot timer 线程，`thread_tcp_handler` 仍无限运行，同时卸载流程已经释放其等待的信号量，
   后续调度可能永久停在残留线程。其他平台仍执行原网络初始化。
7. `chdir()` 的目录 stat 改走 `vfs_path_stat_noflush()`，避免只读目录元数据查询触发所有打开文件的
   写缓冲刷新。
8. 清理 VF2 逐 syscall、设备正常读取、信号 handler 设置、mmap/munmap 成功、lseek、正常 ENOENT、
   ext4 owner/mode 设置成功等高频日志，只保留失败和异常日志。

实机已确认：

```text
F7LY:/$ echo ok
ok
F7LY:/$
```

这说明 BusyBox `execve`、`ppoll`、串口输入、stdin `read`、stdout `writev` 和命令返回提示符均已打通。
首次运行时 BusyBox 还成功创建 `/root/.ash_history`，证明 ext4 用户态写请求能够完成。

上述提示符是恢复 `/root` 初始目录前的实机结果。当前代码正常启动时应显示 `F7LY:/root$`；该项还需
重新执行 `make vf2 shell` 并上板验收。若只能回退到 `/`，串口会打印
`[shell] chdir(/root) failed: <错误码>, fallback to /`。

## 9. 当前验证状态

| 环节 | 状态 | 实机依据 |
| --- | --- | --- |
| U-Boot 加载并跳转 `0x40200000` | 已通过 | 能打印 `E0/E1/S0/S1/M0/P0/P1` |
| 内核主初始化 | 已通过 | F7LY 横幅和 `SYSTEM BOOT COMPLETE` |
| VF2 SD 初始化 | 已通过 | `disk_init done`，可执行后续块读取 |
| MBR 解析 | 已通过 | 正确识别 FAT32 第 1 分区和 Linux 第 2 分区 |
| ext4 只读识别与读取 | 已通过 | 超级块、inode、目录读取成功 |
| ext4 可写挂载 | 已通过本次实机验证 | 8 个扇区写完且 `ext4_sb_write r=0` |
| `dir_init()` | 已通过 | `/dev/misc` 至 `/usr/lib` 初始化完成 |
| 进入用户态 initcode | 已通过 | 评测入口能够运行并访问根文件系统 |
| BusyBox 交互 shell | 已通过 | 从 `/` 启动时出现 `F7LY:/$`，`echo ok` 正常返回 `ok` |
| shell 从 `/root` 启动 | 代码已恢复，待实机确认 | 预期提示符为 `F7LY:/root$`，`pwd` 返回 `/root` |
| VF2 串口输入 | 已通过 | `ppoll -> read` 能接收 PuTTY 按键并由 BusyBox 回显 |
| ext4 用户态写入 | 已通过 | BusyBox 成功创建 `/root/.ash_history`，文件创建/写入链路可用 |
| 评测目录 `/musl`、`/glibc` | 未完成 | 当前 rootfs 中缺少目录，`/musl` 返回 `ENOENT(2)` |
| VF2 网络 | 临时隔离 | VF2 当前跳过 VirtIO/ONPS 初始化，避免失败回滚后残留线程卡住调度 |
| SD 写入后立即读回 | 代码已加入，待实机确认 | 首次真实写请求逐扇区比较原数据与读回数据 |
| SD 重启持久化 | 待验证 | 需要重新上电后检查 ext4 中已创建或修改的内容 |

## 10. 当前仍需整理的内容

1. 上板确认首次 8 个扇区全部打印 `readback verified`，且没有 mismatch、timeout 或数据错误。
2. 重启后检查 ext4 中已经创建或修改的内容，完成持久化验证。
3. 重新构建并上板确认 shell 提示符为 `F7LY:/root$`，且 `pwd`、`cd /`、`cd` 的目录切换结果正确。
4. 验收完成后关闭首次写后读回钩子，并继续精简 SD/ext4 启动阶段日志，只保留必要错误信息。
5. 统一 VF2 各 ext4 挂载入口的只读/可写策略，避免 `vfs_ext_mount2()` 与当前根挂载路径行为不一致。
6. 根据运行目标选择 rootfs：评测模式需要 `/musl`、`/glibc`；交互模式可使用 `make vf2 shell`
   对应的用户态入口和文件系统。
7. 正式修复 ONPS 工作线程生命周期：网卡初始化失败时应先通知并等待所有工作线程退出，再释放锁、
   信号量和协议栈资源；完成前 VF2 继续跳过该网络后端。

## 11. 当前构建与上板命令

WSL 构建：

```bash
cd /mnt/d/OS/F7LY
make vf2
# 或构建交互式 BusyBox shell
make vf2 shell
```

将所选目标生成的 `.bin` 拷贝到 SD 卡 FAT32 第 1 分区并重命名为 `kernel`。交互模式使用
`kernel-rv-visionfive2-shell.bin`。

U-Boot 启动：

```text
mmc dev 1
load mmc 1:1 0x40200000 kernel
go 0x40200000
```

当前正常写挂载的关键日志：

```text
[disk] write ... cnt=8
[sd_write] block done ... words=128 ...
[sd_write] done ...
[ext4_fs_init] after ext4_sb_write state r=0
[fs] 根文件系统已挂载到 /
[fs] after dir_init
```
