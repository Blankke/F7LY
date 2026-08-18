# VisionFive 2 板级适配技术债

## 范围

本文最初基于 `visionfive2` 分支的上板记录，整理 2K1000 适配中不应继续复制的范式。当前主线已经按平台边界吸收 VisionFive 2 的有效增量；下表仍保留历史问题，便于解释为什么不能把旧分支的全部拓扑直接并入主线。

## 2026-08-19 主线收口状态

- 旧分支与主线包含大量内容相同但父提交、顺序不同的提交。功能增量采用单笔 squash 收口，不建立会展开两百余条旧提交的 merge 拓扑。
- 已知可交互版本只管理 `[0x40000000, 0x80000000)` 的 1 GiB 窗口。主线继续把 DTB 作为 RAM 权威来源，同时由 VisionFive 2 画像把当前已验证的 PMM/VMM 管理上限收窄到 `0x80000000`，避免 heap/shm 落到尚未经实机验证的 4 GiB 以上地址。
- GMAC1 的 TX 时钟、门控和复位已由 JH7110 平台控制层完成，不再把 U-Boot 的残留寄存器状态当作必要前提。
- MAC 地址按 GMAC1 物理地址优先读取 U-Boot 修正后的 DTB `local-mac-address`/`mac-address`，再回退到 U-Boot 写入的 MAC address0 寄存器；两个来源都无效时明确失败，不使用多板冲突的固定假地址。串口会输出实际采用的来源和值，仍需在目标 U-Boot 上与开机打印的 EEPROM MAC1 对照验收。
- IPv4、掩码、网关、DNS 和广播地址已经是画像/下板脚本参数，不再写死在驱动中；当前仍是可配置静态地址，没有实现 DHCP。
- 驱动每 250 ms 轮询 PHY，并在 link、speed 或 duplex 变化时重新配置 MAC。当前只验证了 1000 Mbps RGMII 时钟；协商到 100/10 Mbps 时会 fail-closed 并打印原因，低速时钟支持仍是后续实机任务。

### 功能吸收审计

旧分支的提交身份不能直接用于判断功能是否缺失：`git cherry main visionfive2`
能够识别 134 个补丁等价提交，另有 15 个 merge；其余提交中还包含同一批主线改动
被不同顺序重放后产生的新 patch-id。沿 `visionfive2` 的一等父历史，板级专项演进
最终收敛为 `c3f60af0`、`08b5e3c8`、`76f0dbe5`、`b4ca1f94`、
`33f36508`、`f3fa6cff`、`e9a75583`、`a5dc1186` 八笔。审计以这八笔
及其逻辑基线中已经存在的早期 VF2 代码为范围，不以“非祖先提交数”代替功能比较。

| 旧分支的有效能力 | 当前主线等价或增强实现 | 结论 |
| --- | --- | --- |
| `0x40200000` 装载、独立链接布局、进入 C++ 初始化 | `riscv-visionfive2` 画像、专用链接脚本、标准 Linux Image 头、公共 RISC-V 入口和经内容校验的 DTB 参数 | 已吸收；正式契约改为 `booti`，不保留把 `argc/argv` 当启动 ABI 的 `go` 入口。 |
| SATP/页表及板级 MMIO 映射 | 公共 Sv39 页表流程加画像 typed MMIO 资源；VF2 画像把当前已验证管理内存收窄到 `0x80000000` | 已吸收；本次补齐了旧分支已上板成功但主线遗漏的 1 GiB 管理上限。 |
| SBI 串口输出、破坏性 `getchar` 探测后的预读保存、BusyBox 交互 | VF2 console backend、通用 UART `_pending_input`、Console 行规程及 `MODE=shell` 的 `/bin/busybox`/bash/sh 选择 | 已吸收，并保留 `/root` 失败回退 `/` 的交互行为。 |
| SD 卡上电、CMD17/CMD24、FIFO 128-word 边界、错误位、超时、busy 与恢复 | `jh7110_dwmmc` 驱动按单扇区执行 CMD17/CMD24，检查命令/数据错误、FIFO、DATA_OVER、busy、容量和 SDSC/SDHC 地址 | 已吸收；失败写不会自动重放。 |
| 从整盘发现 ext4 分区并可写挂载 | 公共 block 层探测 raw ext4、MBR 和 GPT，验证 ext4 magic 后建立有容量边界的逻辑设备 0 | 已吸收，并删除文件系统反向修改驱动分区偏移的旧耦合。 |
| PLIC 上下文与板级 source 初始化 | 从 DTB `interrupts-extended` 建立 hart/context 映射，平台 IRQ backend 只启用已有 owner 的 source | 已吸收；GMAC1 首阶段仍明确使用轮询。 |
| GMAC1、YT8531、MDIO、TX/RX descriptor 与 ping | `jh7110_gmac` 加 JH7110 平台控制层，包含 PHY 扫描/延时、MAC/DMA 队列、32-bit 连续 DMA 检查和 64-byte cache maintenance | 已吸收，并替代旧驱动对普通 cached BSS/描述符的隐含假设。 |
| 链路变化、TX 时钟、MAC 来源、IPv4 部署参数 | 周期 PHY 状态更新及 MAC 重配；SYSCRG 自初始化；DTB/U-Boot 寄存器 MAC；画像和开发脚本网络变量 | 已吸收或增强；100/10 Mbps 与 DHCP 的剩余边界见上节。 |
| `make vf2`/shell 产物与人工下板 | `make visionfive2`、`make visionfive2-shell` 和 `visionfive2-dev.sh`，使用 `mmc 1:1`、标准地址与 v1.3B DTB | 已吸收，并与 2K1000 的 build/send/串口日志体验对称。 |

以下旧分支内容是排障手段或已知错误，不属于应继续保留的功能提升：常驻
`E0/E1/S0/S1/M0/P0/P1` 标记、首次写请求逐扇区同步读回、正常路径海量
SD/ext4/syscall 日志、固定假 MAC、写死 IPv4、依赖 U-Boot 残留时钟以及
未验证的 100/10 Mbps 时钟写法。主线用分阶段日志和明确失败边界替代这些做法。

## 待整改项

| 问题 | 现有表现与风险 | 2K1000 分支的处理 | VisionFive 2 后续动作 |
| --- | --- | --- | --- |
| 架构与板级配置混在一起 | `Makefile` 的 RISC-V `ARCH_CFLAGS` 默认带 `-DSDCARD`，板级特性会污染所有 RISC-V 目标；构建目录和产物也容易复用错误对象。 | 当前构建系统使用不可拆分的 `PROFILE=loongarch-2k1000` 画像，独立选择板级宏、驱动、构建目录、产物名和链接脚本。 | VF2 已使用 `PROFILE=riscv-visionfive2`，仅在该画像启用 SD、专用链接脚本和外设实现。 |
| 平台判断散落在通用层 | `VISIONFIVE2` 条件分支散布于 boot、VFS、bio、UART 和网络代码，新增板卡会继续扩大分支矩阵。 | 通过 `platform_irq`、`platform_block`、`platform_net_device`、`platform_board` 等门面隔离硬件差异。 | 把 VF2 的 PLIC、SD、console、network 接到同类平台接口，通用 VFS/bio 不再认识板名。 |
| 固件信息未成为权威来源 | VF2 启动代码保留较多固定地址和早期调试特例，DTB 参数没有被系统化校验、解析和集中回退。地址变化时容易静默访问错误硬件。 | 保存固件入口参数，显式区分直接 DTB 与 U-Boot `argc/argv`，按内容校验 FDT；MAC 和内存继续取自 DTB。 | 建立 VF2 启动参数解析器；固定值只能作为有日志的板级 fallback，不能散落在初始化代码。 |
| 分区发现位于文件系统初始化 | VF2 在 `filesystem_init()` 里解析 MBR，再由 `disk_set_partition_offset()` 影响下层 I/O，文件系统与物理介质布局反向耦合。 | 分区发现和逻辑设备 0 映射全部放入块设备门面，VFS/ext4 只看逻辑扇区。 | 将 MBR/GPT/ext4 探测下沉到 VF2 SD 块设备层，并统一容量和越界检查。 |
| 存在两份 UART 状态 | 原通用 console 内嵌一个 `UartManager`，同时系统还有全局 `k_uart`；中断、console 与 `/dev/stdin` 可能操作不同缓冲区，甚至命中未初始化锁。 | console、中断和设备注册统一使用唯一 `dev::k_uart`；本项已在当前分支修正。 | 移植单实例修复；SBI `getchar` 的“探测会消费字符”语义应封装在 console 后端并保留预读字符。 |
| 网络失败通过整个平台跳过 | VF2 为避免 ONPS 初始化失败后残留线程卡住调度，直接跳过真实网络初始化。这掩盖了资源生命周期问题，也阻止未来板载网卡接入。 | 网络栈只依赖板级网卡门面；先完成网卡探测再启动 ONPS，按设备、core、接口三阶段记录状态，接口注册失败时不再调用不安全的线程卸载路径。 | 采用相同分阶段初始化并恢复真实网卡；如果未来需要运行期卸载，还必须补齐全部 ONPS 线程的停止、唤醒、等待和资源释放协议。 |
| 临时诊断侵入正常写路径 | VF2 首次 SD 写请求会同步逐扇区读回，且 SD/ext4 路径保留大量阶段日志。额外 I/O 改变时序和故障面，不能作为正式语义。 | 正常 AHCI 路径不带隐式验收 I/O，只保留错误和关键初始化日志。 | 把读回和详细日志放到显式诊断编译开关或独立维护命令，正式构建默认关闭。 |
| ext4 挂载策略不一致 | VF2 的根挂载路径可写，但 `vfs_ext_mount2()` 仍有板级只读分支，相同文件系统因入口不同而行为不同。 | 平台层只负责块映射，读写策略由统一的文件系统挂载配置决定。 | 删除板级 `ro/rw` 分叉，建立唯一挂载策略和配置入口。 |
| 物理板目标仍沿用运行目标语义 | 构建、复制、QEMU run 与实机启动职责边界不够清楚，容易把物理板产物交给错误运行器。 | 实机统一使用 `make build PROFILE=<实机画像>`；`run/shell/debug` 显式拒绝实机画像。 | 如未来增加 deploy，必须显式指定挂载点且不得隐式覆盖不明设备。 |
| 启动阶段调试标记长期驻留 | `E0/E1/S0/S1/M0/P0/P1` 对首次上板有价值，但长期写死在入口会让启动 ABI、寄存器保存和真实初始化逻辑混杂。 | 入口只完成参数保存、DMW、BSS、逐核栈和 ABI 转换，错误在 C++ 层统一报告。 | 上板稳定后将早期标记收敛到 `EARLY_BOOT_DIAGNOSTICS` 开关，并记录每个标记的寄存器破坏约束。 |
| DMA 一致性没有形成公共约束 | VF2 当前 SD 主路径是 PIO，但代码没有为未来 DMA 描述符建立缓存一致性规范；直接使用普通 BSS 在非一致 DMA 上风险很高。 | 2K1000 链接脚本提供独立 `NOLOAD .dma_uncached`，驱动只通过非缓存 DMW 别名访问 DMA 区。 | 新增 DMA 驱动时必须选择专用非缓存区或完整 cache maintenance API，禁止把普通堆/BSS 地址直接交给设备。 |
| 分支包含大量非板级漂移 | `main...visionfive2` 跨越大量内核、文档、评测和生成物变化，无法把整个分支视作可复用的“板级补丁集”。整体合并会放大回归与审查成本。 | 2K1000 以板级边界组织改动，同时保持默认 LoongArch/QEMU 构建通过。 | VF2 的有效增量已按当前平台边界吸收，并以单笔 squash 收口；旧分支只保留为上板历史，不再整体 merge。 |

## 建议整改顺序

1. 使用 `PROFILE=riscv-visionfive2` 和独立产物，消除全局 `-DSDCARD`。
2. 引入板级 IRQ、block、console、network 门面，将 VFS/bio 中的板名判断下沉。
3. 移植单 UART 修复，并统一 DTB/固件参数契约。
4. 将分区发现下沉，统一 ext4 挂载读写策略。
5. 采用硬件优先、分阶段可重试的 ONPS 初始化后恢复网络；如需运行期卸载，再实现完整线程 join，随后移除首次写回读和常驻启动调试日志。
6. 最后按主题整理 VF2 提交，避免把历史主线漂移带入板级合并。
