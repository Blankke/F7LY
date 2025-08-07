## SD Card 驱动

#### 为什么需要 SD Card 驱动

我们实现的操作系统内核可以烧写在 Visionfive2 开发平板的 SRAM 中，但如果把用户程序也写在内存中的话，可能会有内存空间不足的问题。因此我们希望能将用户程序存储在 SD 卡中，需要运行用户程序时，内核通过读 SD 卡来运行用户程序，也就是说在这种模式下，SD 卡相当于我们平时使用电脑时的磁盘。为了实现读写 SD 卡的功能，我们需要实现 SD Card 驱动。

#### 基本情况

根据 Starfive2 的官方文档，该开发板拥有两个SDIO 接口，其中 SDIO1 用来连接外置 SD 卡。与 K210 平台不同，在 Starfive 平台上，官方并没有提供开发板的 SD 卡驱动代码，因此我们需要自己编写 SD 卡驱动。

阅读 Starfive2 的设备树文件尝试查询 SD 卡接口使用的标准，发现以下字段：

```dtsi
sdio1: sdio1@16020000 {
			compatible = "snps,dw-mshc";
			reg = <0x0 0x16020000 0x0 0x10000>;
			clocks = <&clkgen JH7110_SDIO1_CLK_AHB>,
				 <&clkgen JH7110_SDIO1_CLK_SDCARD>;
			clock-names = "biu","ciu";
			resets = <&rstgen RSTN_U1_DW_SDIO_AHB>;
			reset-names = "reset";
		};
```

根据设备树的该段内容可以得知，sdio 接口使用 snps 公司的 dw-mshc 规范，控制寄存器起始地址为 0x16020000。官方给出了 SDIO 控制寄存器的介绍文档：https://doc-en.rvspace.org/JH7110/TRM/JH7110_TRM/control_registers_sdio.html?hl=sdio，但控制寄存器文档只简单介绍了寄存器的偏移值和功能介绍，具体使用该接口初始化并读写SD卡的流程还需要阅读SNPS(新思科技)的snps_dw_mmc手册。

#### 关键寄存器

##### CTRL

- Size: 32 bits
- Address Offset: 0x0
- 功能：接口控制寄存器，用于设定、重置 SDIO 接口状态

##### CMD

- Size: 32 bits
- Address Offset: 0x2C
- 功能：传输命令寄存器

##### CMDARG

- Size: 32 bits
- Address Offset: 0x28
- 功能：命令参数设置寄存器

##### RESP

- Size: 128 bits
- Address Offset: 0x30
- 功能：响应寄存器

##### RINTSTS

- Size: 32 bits
- Address Offset: 0x44
- 功能：中断状态寄存器，用于查看中断状态，每一位对应一种特定中断

#### FIFO

- Address Offset: >= 0x200
- 功能：FIFO 读写的地址

#### 关于命令

主机对 SD 卡的控制是通过向 SD 卡发送 CMD 来实现的，一条完整的 CMD 指令包含指令序号（Command Index）、指令参数（Argument）以及 CRC 校验位。一条指令执行完成后，SD 卡会向主机回复一条或多条响应。详细内容可阅读 SD Association 官网的文档 _SD Specifications Part 1 Physical Layer Simplified Specification_。

#### 命令发送

接下来将对主机分别在发送非 IO 命令和 IO 命令的情况下，需要完成的操作相应流程进行描述。

##### 非 IO 命令

- 设置 CMDARG 寄存器，写入对应指令需要的参数值。
- 设置 CMD 寄存器，将 CMD_INDEX 设置为对应的指令序号，其他字段则根据对应的指令以及字段的含义进行设置；START_CMD 字段需要置 1，表示该指令开始执行。
- 轮询 CMD 寄存器的 START_CMD 字段直到其被置 0。
- 查询 RINTSTS 寄存器的值，检查是否有错误发生。若没有错误且 RINTSTS 的 Command done 字段为 1，则表示命令执行成功。
- 清空中断寄存器
- 读取 RESP 寄存器的值，即响应内容

##### IO 命令

SD 卡的 IO 命令主要是读指令 CMD17、CMD18 以及写指令 CMD24、CMD25，指令的区别都是单块和多块读写的区别。

- 设置 BLKSIZE、BYTCNT 寄存器，作用是设置一次读写的块的大小以及读写总字节数，其中 BYTCNT 的值需要是 BLKSIZE 的整数倍；在单块读写下，考虑到 SDSC（小容量 SD 卡）单扇区大小为 512 字节，推荐将 BYTCNT 和 BLKSIZE 都设置为 512.
- 设置 CMDARG 寄存器，写入对应指令需要的参数值。IO 指令的参数值对应读写 SD 卡的扇区地址，一个扇区大小为 512 字节。
- 设置 CMD 寄存器，将 CMD_INDEX 设置为对应的指令序号，其他字段则根据对应的指令以及字段的含义进行设置；IO 指令需要将 DATA_EXPECTED 字段置 1，READ_WRITE 字段设置为读/写对应的值；START_CMD 字段需要置 1，表示该指令开始执行。
- 轮询 CMD 寄存器的 START_CMD 字段直到其被置 0。
- 向 FIFO 所在地址写入/读出数据，每次读数据的字长由 FIFO_WIDTH 参数决定。
- 查询 RINTSTS 寄存器的值，检查是否有错误发生。若没有错误且 RINTSTS 的 RXDR 字段或 TXDR 字段为 1，则表示数据读写仍在继续，重复上一步骤操作，直到该字段不再为 1。
- 清空中断寄存器

#### 初始化

在开始使用 SD 卡读写之前，需要进行设置 SDIO 接口响应参数，并完成 SD 卡的初始化命令流程。

首先完成接口相关的初始化流程：

- SD 卡上电：将 PWREN 寄存器的对应位置为 1（只连接一张卡的情况，将 0 号位置 1 即可）。
- 设置 INTMASK 寄存器，不需要屏蔽的中断置 0 即可。将 RINTSTS 寄存器置 0，清空所有中断。
- 设置时钟频率、FIFO 参数

接下来向 SD 卡发送初始化流程指令：

- CMD5，参数为 0x0，若有响应，则为 SDIO 设备（SD 卡肯定没有响应）
- CMD0，参数为 0x0，重置 SD 卡
- CMD8，参数为 0x1aa，若有响应则为 SD2.0 设备。
- 反复发送 ACMD41 指令，参数为 0X40100000（注：ACMD 指令发送前都需要提前发送一个 CMD55 指令），直到返回值的 Bit[31]为 1，则说明初始化成功。
- 发送 CMD3，参数为 0，获取 SD 卡的 RCA 寄存器的值
- 发送 CMD7，参数为 rca 的值，选中当前 SD 卡，并获取 SD 卡当前状态。
- 发送 CMD16，设置 SD 卡 blksize 参数。

至此 SDIO 接口以及 SD 卡的初始化流程完成，可以开始发送 IO 命令读写 SD 卡了。
