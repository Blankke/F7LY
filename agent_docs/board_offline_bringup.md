# 2K1000 与 VisionFive 2 离线下板教程

本文面向第一次独立操作开发板的开发者。目标是：电脑没有互联网时，将
2K1000 和 VisionFive 2 接入同一台路由器，通过 TFTP 下发内核，并通过串口
进入 F7LY Shell。

日常不需要手动输入 U-Boot 命令。仓库脚本会自动等待 U-Boot、设置网络、
下载内核、启动 F7LY，并记录串口日志。

## 1. 固定使用这套网络参数

| 设备 | 固定 IP | 用途 |
| --- | --- | --- |
| 路由器 | `192.168.88.1` | 局域网网关 |
| 开发电脑有线口 | `192.168.88.244` | TFTP 服务器 |
| VisionFive 2 | `192.168.88.243` | VF2 的 U-Boot 和 F7LY |
| 2K1000 | `192.168.88.242` | 2K1000 的 U-Boot 和 F7LY |

共同参数：

```text
子网掩码：255.255.255.0
网关：    192.168.88.1
DNS：     192.168.88.1
广播地址：192.168.88.255
TFTP目录：/srv/tftp
```

板子的 IP 是静态设置，不是 DHCP 分配，所以路由器的“接入设备”页面可能
看不到它们，这是正常现象。

为了避免路由器把这三个地址分给其他设备，建议将 DHCP 地址池限制在例如：

```text
192.168.88.50 ～ 192.168.88.200
```

IP 不能随便填写，必须同时满足：

1. 电脑、两块板和路由器处于同一子网，这里统一为 `192.168.88.0/24`。
2. 每台设备使用不同的 IP。
3. 固定 IP 不在路由器的 DHCP 自动分配范围内。
4. 不能使用网段地址 `.0`、广播地址 `.255` 或网关地址 `.1`。

### 1.1 明天 IP 发生变化时怎么办

板子的 IP 不是从路由器自动获取后再“查出来”的，而是运行下板脚本时由
用户主动指定的静态 IP。脚本会在 U-Boot 中设置 `ipaddr`，构建时还会把
相同地址写入 F7LY 内核网络配置。

明天先执行：

```bash
ip -br -4 addr
ip -4 route
```

找到连接路由器的有线网卡，不要选择 Wi-Fi、Docker 网桥或 VPN 地址。假如
看到：

```text
enx...  UP  192.168.5.17/24
default via 192.168.5.1
```

就可以使用：

| 设备或参数 | 地址 |
| --- | --- |
| 电脑/TFTP 服务器 | `192.168.5.17` |
| 路由器/网关 | `192.168.5.1` |
| VisionFive 2 | `192.168.5.243` |
| 2K1000 | `192.168.5.242` |
| 子网掩码 | `255.255.255.0` |
| 广播地址 | `192.168.5.255` |

这里的 `/24` 表示子网掩码是 `255.255.255.0`，所以电脑和开发板地址的前三
段必须相同。`.242` 和 `.243` 只是推荐的空闲地址，不是开发板硬件规定的
固定值。选择地址时还要确认它们不在路由器的 DHCP 地址池内，也没有被其他
设备占用。

可以在两块板断电时先做初步检查：

```bash
ping -c 1 192.168.5.242
ping -c 1 192.168.5.243
```

有回复说明地址已被占用，必须换一个。没有回复只是初步判断，因为某些设备
可能禁止响应 ping；最终仍应结合路由器 DHCP 列表和固定地址规划确认。

网络改为上述 `192.168.5.0/24` 后，VisionFive 2 使用：

```bash
BOARD_IP=192.168.5.243 \
SERVER_IP=192.168.5.17 \
NETMASK=255.255.255.0 \
GATEWAY_IP=192.168.5.1 \
DNS_IP=192.168.5.1 \
BROADCAST_IP=192.168.5.255 \
SERIAL_DEVICE=/dev/ttyUSB0 \
TFTP_DIR=/srv/tftp \
./scripts/board/visionfive2-dev.sh build
```

2K1000 使用：

```bash
BOARD_IP=192.168.5.242 \
SERVER_IP=192.168.5.17 \
NETMASK=255.255.255.0 \
GATEWAY_IP=192.168.5.1 \
DNS_IP=192.168.5.1 \
BROADCAST_IP=192.168.5.255 \
SERIAL_DEVICE=/dev/ttyUSB1 \
TFTP_DIR=/srv/tftp \
./scripts/board/2k1000-dev.sh build
```

如果只想查看 U-Boot 当前保存的临时地址，可以在 U-Boot 提示符执行：

```console
printenv ipaddr serverip netmask
```

其中：

```text
ipaddr   = 开发板自己的 IP
serverip = 开发电脑的 TFTP IP
netmask  = 子网掩码
```

这些旧值不决定下一次脚本使用什么地址，因为脚本会重新执行 `setenv`，而且
不会执行 `saveenv`。

网络参数变化时遵循下面的规则：

- `BOARD_IP`、`NETMASK`、`GATEWAY_IP`、`DNS_IP` 或 `BROADCAST_IP` 变化，
  必须先执行一次 `build`，因为它们会写入 F7LY 内核。
- 只有电脑的 `SERVER_IP` 在同一子网内变化，而板子的网络参数完全没变时，
  可以给 `send` 传入新的 `SERVER_IP`。
- 不确定当前 `.bin` 使用了哪套参数时，直接执行 `build`。

最省事的做法仍然是将电脑有线口固定为 `192.168.88.244/24`，并把路由器
DHCP 范围设置为不包含 `.242`、`.243` 和 `.244`。这样第二天只需重新确认
两个串口设备名，不需要修改网络参数。

## 2. 正确连接硬件

三台设备全部接到路由器的 LAN 口，不要接 WAN 口：

```text
开发电脑有线口 ── 路由器 LAN
VisionFive 2网口 ── 路由器 LAN
2K1000网口 ───── 路由器 LAN
```

路由器没有互联网也没关系。下板过程只使用局域网：

```text
电脑 ──TFTP──> 开发板
电脑 <──串口──> 开发板
```

TFTP 只负责把内核和 DTB 搬到开发板内存，不能替代根文件系统：

- VisionFive 2 仍然从 microSD 卡读取 rootfs。
- 2K1000 仍然从 SATA 固态硬盘读取 rootfs。
- 两块盘都必须提前准备好，复位或重新下发内核时不需要重新写盘。

### 2.1 VisionFive 2 串口

使用 3.3V USB-TTL，只接三根线：

| VisionFive 2 | USB-TTL |
| --- | --- |
| GND | GND |
| TX | RXD |
| RX | TXD |

不要连接 VCC，不要使用 5V TTL。VisionFive 2 使用自己的电源供电。

### 2.2 2K1000 串口

使用板载 USB Debug/UART 接口。不要照搬 VisionFive 2 的针脚接线。如果使用
DB9 接口，必须先确认电平类型，RS-232 不能直接连接 3.3V USB-TTL。

## 3. 确认两个串口设备

两根串口都插上后执行：

```bash
find /dev -maxdepth 1 \
  \( -name 'ttyUSB*' -o -name 'ttyACM*' \) \
  -print
```

可能得到：

```text
/dev/ttyUSB0
/dev/ttyUSB1
```

设备编号可能因为重启或插拔顺序改变。最简单的识别办法：

1. 拔掉两根串口。
2. 只插 2K1000 串口，执行一次上面的命令并记下设备名。
3. 再插 VisionFive 2 串口，再执行一次。
4. 新出现的设备就是 VisionFive 2。
5. 第二天重新连接后再次确认，不要永久假设编号不变。

后文示例假设：

```text
VisionFive 2：/dev/ttyUSB0
2K1000：      /dev/ttyUSB1
```

如果实际结果相反，只交换命令中的 `SERIAL_DEVICE`，不要修改 IP。

## 4. 断网前检查电脑

### 4.1 检查电脑有线地址

```bash
ip -br -4 addr
```

连接路由器的有线网卡必须包含：

```text
192.168.88.244/24
```

不要把 Wi-Fi 地址填入 `SERVER_IP`。本项目的 TFTP 服务器地址固定使用电脑
有线口的 `192.168.88.244`。

检查路由器：

```bash
ping -c 1 192.168.88.1
```

### 4.2 检查 TFTP

```bash
grep TFTP_DIRECTORY /etc/default/tftpd-hpa
sudo systemctl enable --now tftpd-hpa
sudo systemctl status tftpd-hpa --no-pager
ls -lh /srv/tftp
```

服务状态应为 `active (running)`，目录中至少应该有：

```text
kernel-la-2k1000-shell.bin
kernel-rv-visionfive2-shell.bin
jl-lsgd2k10.dtb
```

确认 UDP 69 正在监听：

```bash
sudo ss -lunp 'sport = :69'
```

防火墙必须允许 TFTP：

```bash
sudo ufw allow 69/udp
```

如果没有使用 UFW，不要为了执行这条命令额外安装它。

### 4.3 检查工具链和镜像

```bash
command -v minicom
command -v riscv64-linux-gnu-gcc
command -v loongarch64-linux-gnu-gcc

ls -lh \
  images/oscomp-final-riscv64.img \
  images/oscomp-final-loongarch64.img
```

这些文件和命令必须在断网前准备好。正常的 `make build` 不需要互联网。

## 5. 每天实际怎么操作

第一次建议一块一块启动。确认第一块进入 Shell 后退出 minicom，再启动
第二块。

退出 minicom：

```text
先按 Ctrl-A，再按 X，最后确认退出
```

### 5.1 VisionFive 2：修改代码后重新编译并下板

假设 VF2 串口是 `/dev/ttyUSB0`：

```bash
BOARD_IP=192.168.88.243 \
SERVER_IP=192.168.88.244 \
NETMASK=255.255.255.0 \
GATEWAY_IP=192.168.88.1 \
DNS_IP=192.168.88.1 \
BROADCAST_IP=192.168.88.255 \
SERIAL_DEVICE=/dev/ttyUSB0 \
TFTP_DIR=/srv/tftp \
./scripts/board/visionfive2-dev.sh build
```

脚本打开 minicom 后，如果板子没有自动进入 U-Boot，按一次 RESET。不要连续
反复按 RESET。

脚本会自动完成：

1. 等待并截停 U-Boot。
2. 搬移和校验板载 DTB。
3. 设置板子 IP 和 TFTP 服务器 IP。
4. 测试局域网连通性。
5. 从电脑下载内核。
6. 执行 `booti`。
7. 保留 minicom 供用户操作 F7LY Shell。

### 5.2 VisionFive 2：复用当前内核直接下板

只有在代码和网络参数都没有变化时才使用：

```bash
BOARD_IP=192.168.88.243 \
SERVER_IP=192.168.88.244 \
SERIAL_DEVICE=/dev/ttyUSB0 \
TFTP_DIR=/srv/tftp \
./scripts/board/visionfive2-dev.sh send
```

### 5.3 2K1000：修改代码后重新编译并下板

假设 2K1000 串口是 `/dev/ttyUSB1`：

```bash
BOARD_IP=192.168.88.242 \
SERVER_IP=192.168.88.244 \
NETMASK=255.255.255.0 \
GATEWAY_IP=192.168.88.1 \
DNS_IP=192.168.88.1 \
BROADCAST_IP=192.168.88.255 \
SERIAL_DEVICE=/dev/ttyUSB1 \
TFTP_DIR=/srv/tftp \
./scripts/board/2k1000-dev.sh build
```

脚本打开 minicom 后，如果板子没有自动进入 U-Boot，按一次 RESET。

脚本会自动完成：

1. 等待并截停 U-Boot。
2. 设置板子 IP 和 TFTP 服务器 IP。
3. 测试局域网连通性。
4. 下载并校验 2K1000 DTB。
5. 下载内核。
6. 执行 `go`。
7. 保留 minicom 供用户操作 F7LY Shell。

### 5.4 2K1000：复用当前内核直接下板

只有在代码和网络参数都没有变化时才使用：

```bash
BOARD_IP=192.168.88.242 \
SERVER_IP=192.168.88.244 \
SERIAL_DEVICE=/dev/ttyUSB1 \
TFTP_DIR=/srv/tftp \
./scripts/board/2k1000-dev.sh send
```

## 6. `build` 和 `send` 的区别

`build` 会执行：

```text
编译代码 → 发布到 TFTP → 打开串口 → 自动下板
```

`send` 会执行：

```text
复用上次的内核 → 发布到 TFTP → 打开串口 → 自动下板
```

以下情况必须使用 `build`：

- 修改了内核代码。
- 修改了板子的 IP、网关、DNS 或广播地址。
- 切换了 Git commit。
- 不确定当前 `.bin` 是否对应最新源码。

`send` 不会重新编译。即使 `send` 临时修改了 U-Boot 的 `BOARD_IP`，内核启动
后仍可能使用上一次构建时写入的旧 IP。因此修改网络参数后必须先执行一次
`build`。

## 7. 参数是什么意思

### 7.1 `SERVER_IP`

运行 TFTP 服务的开发电脑有线地址：

```text
192.168.88.244
```

### 7.2 `BOARD_IP`

开发板自己的静态地址：

```text
VisionFive 2：192.168.88.243
2K1000：      192.168.88.242
```

两块板不能相同，也不能和电脑或路由器相同。

### 7.3 `SERIAL_DEVICE`

脚本操作的串口设备，例如：

```text
/dev/ttyUSB0
/dev/ttyUSB1
```

串口负责显示和键盘输入；网口负责下载内核。两者互不替代。

### 7.4 `TFTP_DIR`

电脑上存放下板文件的目录：

```text
/srv/tftp
```

脚本会自动把编译结果复制到这里。

### 7.5 `ETHACT`

`ETHACT` 表示 U-Boot 使用哪个板载网口。VisionFive 2 当前已验证为：

```text
ethernet@16040000
```

2K1000 有两个板载网口。优先使用已经验证成功的网口，通常是靠近电源的
网口。当前脚本使用 U-Boot 默认选择的网口；如果 `ping` 失败，再检查插线
位置和 U-Boot 的 `ethact`。

### 7.6 内核和 DTB 地址

这些是开发板内存地址，不是 IP，不要修改：

```text
VisionFive 2 内核：0x40200000
VisionFive 2 DTB： 0x46000000

2K1000 内核：0x9000000000200000
2K1000 DTB： 0x900000000a000000
```

## 8. 成功时应该看到什么

VisionFive 2 会经过：

```text
Starting kernel ...
[boot] early runtime begin
[dwmmc] ... card ready ...
[fs] 根文件系统已挂载到 /
#### F7LY INTERACTIVE SHELL START ####
F7LY:...$
```

2K1000 会经过：

```text
[early] entry reached
[pmm] ...
[ahci] ... SATA disk ready ...
[fs] 根文件系统已挂载到 /
#### F7LY INTERACTIVE SHELL START ####
F7LY:...$
```

进入 Shell 后先执行简单的只读检查：

```sh
pwd
ls /
```

## 9. 失败时只检查对应的一层

### 9.1 脚本一直等待

- 确认选择了正确的 `SERIAL_DEVICE`。
- 按一次开发板 RESET。
- 检查串口是否被另一个 minicom 占用。

### 9.2 `ping` 失败

- 确认电脑和两块板都接在路由器 LAN 口。
- 确认电脑有线地址是 `192.168.88.244/24`。
- 确认两块板没有使用相同 IP。
- 2K1000 尝试另一个板载网口。
- 检查电脑防火墙。

### 9.3 `File not found` 或 TFTP 失败

```bash
ls -lh /srv/tftp
sudo systemctl status tftpd-hpa --no-pager
sudo ss -lunp 'sport = :69'
```

### 9.4 内核 panic 或卡住

查看脚本自动记录的最新串口日志：

```bash
tail -n 100 logs/run/output_visionfive2_latest.txt
tail -n 100 logs/run/output_2k1000_latest.txt
```

不要只保存最后一行 panic。排查问题时需要从 U-Boot 下载内核开始的完整日志。

### 9.5 根文件系统失败

- VisionFive 2 检查 microSD 卡和 ext4 根分区。
- 2K1000 检查 SATA 数据线、供电、port 0 和整盘 ext4 镜像。
- TFTP 成功不代表根盘正常，这两部分是独立的。

## 10. 明天开始开发前的最短检查清单

1. 路由器、电脑、两块开发板全部接 LAN 口。
2. VF2 插好 microSD；2K1000 插好 SATA 盘。
3. 两块板分别接好串口并确认 `/dev/ttyUSB*` 对应关系。
4. 执行 `ip -br -4 addr`，确认电脑有线口为 `192.168.88.244/24`。
5. 执行 `systemctl is-active tftpd-hpa`，确认输出 `active`。
6. 修改代码后执行对应脚本的 `build`。
7. minicom 打开后按一次目标开发板的 RESET。
8. 等待出现 `F7LY:...$`。
9. 出错时读取 `logs/run/output_*_latest.txt`，不要凭感觉反复改参数。
