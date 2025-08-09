#include "fs/drivers/riscv/sdcard.hh"
#include "printer.hh"
#include "types.hh"

static struct sdio_context_t
{
  void (*wake_evt)(SDMMC_T *pSDMMC, uint32 event, uint32 arg);
  uint32 (*wait_evt)(SDMMC_T *pSDMMC, uint32 event, uint32 arg);
  uint32 flag;
  uint32 response[4];
  int fnum;
  uint16 blkSz[8]; /* Block size setting for the 8- function blocks */
} sdio_context;
static struct sdio_context_t *sdioif = &sdio_context;

enum SDIO_STATE
{
  SDIO_STATE_IDLE,      /**! SDIO Driver IDLE */
  SDIO_STATE_CMD_WAIT,  /**! SDIO Driver waiting for CMD complete */
  SDIO_STATE_CMD_DONE,  /**! SDIO Command completed successfully */
  SDIO_STATE_CMD_ERR,   /**! SDIO Command transfer error */
  SDIO_STATE_DATA_WAIT, /**! SDIO Driver waiting for data complete */
  SDIO_STATE_DATA_DONE, /**! SDIO data completed successfully */
  SDIO_STATE_DATA_ERR,  /**! SDIO data transfer error */
};

/*****************************************************************************
 * Private functions
 ****************************************************************************/
static struct sdio_state
{
  enum SDIO_STATE cstate;
  enum SDIO_STATE dstate;
  uint32 carg;
  uint32 darg;
} sstate;

static volatile uint32 card_int;

static uint32 SDIO_WaitEvent(SDMMC_T *pSDMMC, uint32 event, uint32 arg)
{
  uint32 ret = 0;
  // printf("%d", arg);
  switch (event)
  {
  case SDIO_START_COMMAND:
    sstate.cstate = SDIO_STATE_CMD_WAIT;
    break;

  case SDIO_START_DATA:
    sstate.dstate = SDIO_STATE_DATA_WAIT;
    break;

  case SDIO_WAIT_COMMAND:
    while (*((volatile enum SDIO_STATE *)&sstate.cstate) ==
           SDIO_STATE_CMD_WAIT)
    {
      // __WFI();
      wait_for_sdio_irq(pSDMMC);
      printfPink("WFI\n");
    }
    ret = sstate.carg;
    break;

  case SDIO_WAIT_DATA:
    while (*((volatile enum SDIO_STATE *)&sstate.dstate) ==
           SDIO_STATE_DATA_WAIT)
    {
      // __WFI();
      wait_for_sdio_irq(pSDMMC);
      printfPink("WFI\n");
    }
    ret = sstate.darg;
    break;

  case SDIO_WAIT_DELAY:
  {
    // uint32 cntr = ms_cnt + arg;
    // while (cntr > ms_cnt) {
    // 	printf("cntr: %d ms_cnt: %d", cntr, ms_cnt);
    // }
    while (arg)
    {
      arg--;
    }

    break;
  }
  default:
    break;
  }
  return ret;
}

static void SDIO_WakeEvent(SDMMC_T *pSDMMC, uint32 event, uint32 arg)
{
  switch (event)
  {
  case SDIO_CARD_DETECT:
    /* Handle Card Detect here */
    break;

  /* SDIO Command transmitted successfully */
  case SDIO_CMD_DONE:
    sstate.cstate = SDIO_STATE_CMD_DONE;
    sstate.carg = 0;
    break;

  /* SDIO Command has errors 'arg' has more info */
  case SDIO_CMD_ERR:
    sstate.cstate = SDIO_STATE_CMD_ERR;
    sstate.carg = arg;
    break;

  /* Error in data transfer */
  case SDIO_DATA_ERR:
    sstate.dstate = SDIO_STATE_DATA_ERR;
    sstate.darg = arg;
    break;

  /* Data transfer completed successfully */
  case SDIO_DATA_DONE:
    sstate.dstate = SDIO_STATE_DATA_DONE;
    sstate.darg = 0;
    break;

  /* Interrupt from SDIO card function */
  case SDIO_CARD_INT:
    card_int = 1;
    break;

  default:
    break;
  }
}

/**
 * @brief 初始化SDMMC平台硬件
 *
 * 配置SDMMC控制器的基本设置：
 * 1. 软件复位所有模块
 * 2. 清除所有中断
 * 3. 启用全局中断
 * 4. 设置超时值
 * 5. 配置FIFO和时钟
 *
 * @param pSDMMC SDMMC控制器指针
 * @return 始终返回0
 */
int platform_init(SDMMC_T *pSDMMC)
{
  /* Enable SDIO module clock */
  // Chip_Clock_EnableOpts(CLK_MX_SDIO, true, true, 1);

  /* Software reset */
  pSDMMC->BMOD = MCI_BMOD_SWR; // 软件复位内部寄存器
  /* reset all blocks */
  pSDMMC->CTRL = MCI_CTRL_RESET | MCI_CTRL_FIFO_RESET | MCI_CTRL_DMA_RESET; // 复位控制器、FIFO和DMA
  while (pSDMMC->CTRL &
         (MCI_CTRL_RESET | MCI_CTRL_FIFO_RESET | MCI_CTRL_DMA_RESET))
  { // 等待复位完成
    printfPink("ctrl:%p\n", pSDMMC->CTRL);
  }

  /* Clear the interrupts for the host controller */
  pSDMMC->PWREN = 1;            // 使能卡槽电源
  pSDMMC->RINTSTS = 0xFFFFFFFF; // 清除所有中断状态

  /* Internal DMA setup for control register */
  // pSDMMC->CTRL = MCI_CTRL_USE_INT_DMAC | MCI_CTRL_INT_ENABLE;
  pSDMMC->CTRL = MCI_CTRL_INT_ENABLE; // 使能全局中断
  pSDMMC->INTMASK = 0;                // 禁用所有中断掩码

  /* Put in max timeout */
  pSDMMC->TMOUT = 0xFFFFFFFF; // 设置最大超时值

  /* FIFO threshold settings for DMA, DMA burst of 4,   FIFO watermark at 16 */
  // pSDMMC->FIFOTH = MCI_FIFOTH_DMA_MTS_4 | MCI_FIFOTH_RX_WM((SD_FIFO_SZ / 2) -
  // 1) | MCI_FIFOTH_TX_WM(SD_FIFO_SZ / 2);

  /* Enable internal DMA, burst size of 4, fixed burst */
  // pSDMMC->BMOD = MCI_BMOD_DE | MCI_BMOD_PBL4 | MCI_BMOD_DSL(4);

  /* disable clock to CIU (needs latch) */
  pSDMMC->CLKENA = 0;
  pSDMMC->CLKSRC = 0;
  return 0;
}
int Platform_CardNDetect(SDMMC_T *pSDMMC) { return (pSDMMC->CDETECT & 1); }

void SD_IRQHandler(SDMMC_T *pSDMMC)
{
  uint32 status = pSDMMC->MINTSTS;
  uint32 iclr = 0;

  /* Card Detected */
  if (status & 1)
  {
    sdioif->wake_evt(pSDMMC, SDIO_CARD_DETECT, 0);
    iclr = 1;
  }

  /* Command event error */
  if (status & (SDIO_CMD_INT_MSK & ~4))
  {
    sdioif->wake_evt(pSDMMC, SDIO_CMD_ERR, (status & (SDIO_CMD_INT_MSK & ~4)));
    iclr |= status & SDIO_CMD_INT_MSK;
  }
  else if (status & 4)
  {
    /* Command event done */
    sdioif->wake_evt(pSDMMC, SDIO_CMD_DONE, status);
    iclr |= status & SDIO_CMD_INT_MSK;
  }

  /* Command event error */
  if (status & (SDIO_DATA_INT_MSK & ~8))
  {
    sdioif->wake_evt(pSDMMC, SDIO_DATA_ERR, status & (SDIO_DATA_INT_MSK & ~8));
    iclr |= (status & SDIO_DATA_INT_MSK) | (3 << 4);
  }
  else if (status & 8)
  {
    /* Command event done */
    sdioif->wake_evt(pSDMMC, SDIO_DATA_DONE, status);
    iclr |= (status & SDIO_DATA_INT_MSK) | (3 << 4);
  }

  /* Handle Card interrupt */
  if (status & SDIO_CARD_INT_MSK)
  {
    sdioif->wake_evt(pSDMMC, SDIO_CARD_INT, 0);
    iclr |= status & SDIO_CARD_INT_MSK;
  }

  /* Clear the interrupts */
  pSDMMC->RINTSTS = iclr;
}

/**
 * @brief 等待SDIO中断发生并处理
 *
 * 轮询原始中断状态寄存器(RINTSTS)，直到检测到中断，然后调用中断处理函数
 * 0xffff0004 = 命令完成中断(bit 2) + 其他错误中断掩码
 *
 * @param pSDMMC SDMMC控制器指针
 * @return 始终返回0
 */
uint32 wait_for_sdio_irq(SDMMC_T *pSDMMC)
{
  uint32 rintst;
  while (1)
  {
    rintst = pSDMMC->RINTSTS; // 读取原始中断状态寄存器
    // printf("rintst: %p\n", rintst);
    if (rintst & 0xffff0004)
    { // 检查是否有命令完成或错误中断
      break;
    }
  }
  SD_IRQHandler(pSDMMC); // 处理中断
  return 0;
}

/**
 * @brief 向SD卡发送命令并等待响应
 *
 * 该函数是高级命令发送接口，包含完整的命令发送流程：
 * 1. 设置中断掩码，根据是否为数据命令启用相应中断
 * 2. 发送命令到SD卡
 * 3. 等待命令完成中断
 * 4. 清除中断状态
 * 5. 读取响应数据
 *
 * @param pSDMMC SDMMC控制器指针
 * @param cmd 命令字，包含命令索引和选项标志位
 * @param arg 命令参数
 * @return 命令执行结果，0表示成功
 */
uint32 SD_Send_Command(SDMMC_T *pSDMMC, uint32 cmd, uint32 arg)
{
  uint32 ret = 0, ival;
  uint32 imsk = pSDMMC->INTMASK;
  // ret = sdioif->wait_evt(pSDMMC, SDIO_START_COMMAND, (cmd & 0x3F));
  // ival = SDIO_CMD_INT_MSK & ~ret;
  ival = SDIO_CMD_INT_MSK; // 0xA146: 命令相关中断掩码

  /* 如果是数据传输命令，则启用数据传输中断 */
  if (cmd & SDIO_CMD_DATA)
  {                            // 检查命令是否包含数据传输
    ival |= SDIO_DATA_INT_MSK; // 0xBE88: 添加数据传输中断掩码
    imsk |= SDIO_DATA_INT_MSK;
  }

  SD_SetIntMask(pSDMMC, ival);  // 设置中断掩码寄存器
  SD_SendCmd(pSDMMC, cmd, arg); // 发送命令到SD卡
  // ret = sdioif->wait_evt(pSDMMC, SDIO_WAIT_COMMAND, 0);
  wait_for_sdio_irq(pSDMMC);    // 等待命令完成中断
  pSDMMC->RINTSTS = 0xFFFFFFFF; // 清除所有中断状态
  // if (!ret && (cmd & SDIO_CMD_RESP_R1)) {
  // 	Chip_SDIF_GetResponse(pSDMMC, &sdioif->response[0]);
  // }

  SD_GetResponse(pSDMMC, &sdioif->response[0]); // 读取SD卡响应
  SD_SetIntMask(pSDMMC, imsk);                  // 恢复原始中断掩码
  return ret;
}

/**
 * @brief 读取SD卡命令响应数据
 *
 * 从4个响应寄存器(RESP0-RESP3)中读取SD卡对命令的响应
 *
 * @param pSDMMC SDMMC控制器指针
 * @param resp 用于存储响应数据的4个32位数组
 */
void SD_GetResponse(SDMMC_T *pSDMMC, uint32 *resp)
{
  /* on this chip response is not a fifo so read all 4 regs */
  resp[0] = pSDMMC->RESP0; // 读取响应寄存器0
  resp[1] = pSDMMC->RESP1; // 读取响应寄存器1
  resp[2] = pSDMMC->RESP2; // 读取响应寄存器2
  resp[3] = pSDMMC->RESP3; // 读取响应寄存器3
}

/**
 * @brief 设置SD卡接口类型
 *
 * 配置卡类型寄存器，设置数据总线宽度：
 * - 0: 1位模式
 * - 1: 4位模式
 * - 0x10000: 8位模式
 *
 * @param pSDMMC SDMMC控制器指针
 * @param ctype 卡类型配置值
 */
void SD_SetCardType(SDMMC_T *pSDMMC, uint32 ctype)
{
  pSDMMC->CTYPE = ctype;
}

/**
 * @brief 设置SD卡时钟频率
 *
 * 根据系统时钟频率和目标速度计算分频比，设置SD卡时钟：
 * 1. 先禁用时钟
 * 2. 设置分频器
 * 3. 通知CIU更新时钟配置
 * 4. 启用时钟
 *
 * @param pSDMMC SDMMC控制器指针
 * @param clk_rate 系统时钟频率
 * @param speed 目标SD卡时钟频率
 */
void SD_SetClock(SDMMC_T *pSDMMC, uint32 clk_rate, uint32 speed)
{
  /* compute SD/MMC clock dividers */
  uint32 div;

  div = ((clk_rate / speed) + 2) >> 1; // 计算分频值

  if ((div == pSDMMC->CLKDIV) && pSDMMC->CLKENA)
  {
    return; /* Closest speed is already set */
  }
  /* disable clock */
  pSDMMC->CLKENA = 0; // 禁用时钟

  /* User divider 0 */
  pSDMMC->CLKSRC = MCI_CLKSRC_CLKDIV0; // 选择分频器0

  /* inform CIU */
  SD_SendCmd(pSDMMC, MCI_CMD_UPD_CLK | MCI_CMD_PRV_DAT_WAIT, 0); // 通知CIU更新时钟

  /* set divider 0 to desired value */
  pSDMMC->CLKDIV = MCI_CLOCK_DIVIDER(0, div); // 设置分频值

  /* inform CIU */
  SD_SendCmd(pSDMMC, MCI_CMD_UPD_CLK | MCI_CMD_PRV_DAT_WAIT, 0); // 通知CIU更新时钟

  /* enable clock */
  pSDMMC->CLKENA = MCI_CLKEN_ENABLE; // 启用时钟

  /* inform CIU */
  SD_SendCmd(pSDMMC, MCI_CMD_UPD_CLK | MCI_CMD_PRV_DAT_WAIT, 0); // 通知CIU更新时钟
}

/**
 * @brief 设置中断掩码
 *
 * 配置INTMASK寄存器，控制哪些中断被使能
 *
 * @param pSDMMC SDMMC控制器指针
 * @param iVal 中断掩码值，1表示使能对应中断
 */
void SD_SetIntMask(SDMMC_T *pSDMMC, uint32 iVal)
{
  pSDMMC->INTMASK = iVal;
}

/**
 * @brief 发送命令到SD卡(底层接口)
 *
 * 向SD卡发送底层命令，包括：
 * 1. 设置命令参数寄存器
 * 2. 启动命令传输
 * 3. 轮询等待CIU接受命令
 *
 * @param pSDMMC SDMMC控制器指针
 * @param cmd 命令字，包含命令索引和控制标志
 * @param arg 命令参数
 * @return 0表示成功，1表示超时
 */
int SD_SendCmd(SDMMC_T *pSDMMC, uint32 cmd, uint32 arg)
{
  volatile int tmo = 50; // 超时计数器
  volatile int delay;

  /* set command arg reg*/
  pSDMMC->CMDARG = arg;              // 设置命令参数寄存器
  pSDMMC->CMD = MCI_CMD_START | cmd; // 启动命令，MCI_CMD_START设置bit31启动命令

  /* poll untill command is accepted by the CIU */
  while (--tmo && (pSDMMC->CMD & MCI_CMD_START))
  { // 等待CIU接受命令(START位被清除)
    if (tmo & 1)
    {
      delay = 50;
    }
    else
    {
      delay = 18000;
    }

    while (--delay > 1)
    { // 延时等待
    }
  }

  return (tmo < 1) ? 1 : 0; // 返回结果：0成功，1超时
}

/**
 * @brief SD卡初始化流程
 *
 * 按照SD卡标准初始化序列初始化SD卡：
 * 1. 设置初始时钟频率(通常400KHz)
 * 2. 发送CMD5检测SDIO设备(SD卡无响应)
 * 3. 发送CMD0复位SD卡
 * 4. 发送CMD8检测SD2.0设备
 * 5. 循环发送ACMD41直到初始化完成
 * 6. 发送CMD2获取CID
 * 7. 发送CMD3获取RCA
 * 8. 设置高速时钟(25MHz)
 *
 * @param pSDMMC SDMMC控制器指针
 * @param freq 初始时钟频率
 * @return SD卡的RCA(相对卡地址)
 */
uint32 SD_Card_Init(SDMMC_T *pSDMMC, uint32 freq)
{
  uint32 val;
  uint32 rca;
  /* Set Clock to 400KHz */
  SD_SetCardType(pSDMMC, 0);            // 设置为1位模式
  SD_SetClock(pSDMMC, 200000000, freq); // 设置初始时钟频率
  printfPink("arrive a0\n");
  sdioif->wait_evt(pSDMMC, SDIO_WAIT_DELAY, 100); /* Wait for card to wake up */

  printfPink("arrive a\n");
  printfPink("RINTSTS: %p\n", pSDMMC->RINTSTS);
  printfPink("responese: %p\n", pSDMMC->RESP0);
  // CMD5: 检测SDIO设备，SD卡不会响应
  SD_Send_Command(pSDMMC, CMD5, 0);
  // if (ret) return ret;
  val = sdioif->response[0];
  printfPink("CMD5 response: %p\n", val);
  printfPink("arrive b\n");

  printfPink("RINTSTS: %p\n", pSDMMC->RINTSTS);
  // CMD0: 重置SD卡到idle状态
  SD_Send_Command(pSDMMC, CMD0, 0);
  // if (ret) return ret;
  val = sdioif->response[0];
  printfPink("CMD0 response: %p\n", val);

  printfPink("RINTSTS: %p\n", pSDMMC->RINTSTS);

  // CMD8: 检测SD2.0设备，参数0x1aa用于电压检测
  SD_Send_Command(pSDMMC, CMD8, 0x1aa);
  val = sdioif->response[0];
  printfPink("CMD8 response: %p\n", val);
  printfPink("arrive c\n");

  // 循环发送ACMD41，直到SD卡初始化完成(bit31置1)
  do
  {
    SD_Send_Command(pSDMMC, CMD55, 0);           // CMD55: 下一个命令是应用专用命令
    SD_Send_Command(pSDMMC, ACMD41, 0x40100000); // ACMD41: 初始化命令，0x40100000表示高容量支持
    val = sdioif->response[0];
    printf("response: %p\n", val);
  } while ((val & 0x80000000) == 0); // 等待bit31置1，表示初始化完成

  printfPink("response: %p\n", val);
  printfPink("arrive d\n");

  // CMD2: 获取CID(卡标识信息)，所有卡响应
  SD_Send_Command(pSDMMC, CMD2, 0);
  printfPink("response3: %p\n", sdioif->response[3]);
  printfPink("response3: %p\n", sdioif->response[2]);
  printfPink("response3: %p\n", sdioif->response[1]);
  printfPink("response3: %p\n", sdioif->response[0]);
  printfPink("arrive e\n");

  // CMD3: 请求SD卡发布RCA(相对卡地址)
  SD_Send_Command(pSDMMC, CMD3, 0);
  val = sdioif->response[0];
  printfPink("response: %p\n", val);
  printfPink("arrive e\n");

  rca = (val & 0xffff0000); // 提取RCA地址(高16位)
  printfPink("rca: %p\n", rca);

  // 初始化完成，设置高速时钟25MHz
  SD_SetClock(pSDMMC, 200000000, 25000000);
  return rca;
}

/**
 * @brief 设置SD卡块大小
 *
 * 选择SD卡并设置数据块大小：
 * 1. 发送CMD7选择指定RCA的SD卡
 * 2. 发送CMD16设置块大小
 * 3. 配置控制器块大小寄存器
 *
 * @param pSDMMC SDMMC控制器指针
 * @param blkSize 块大小(通常为512字节)
 * @param rca SD卡的相对地址
 * @return 始终返回0
 */
int SD_Card_SetBlockSize(SDMMC_T *pSDMMC, uint32 blkSize, uint32 rca)
{
  uint32 val;
  // CMD7: 选择/取消选择SD卡，参数为RCA
  SD_Send_Command(pSDMMC, CMD7, rca);
  val = sdioif->response[0];
  printfPink("response: %p\n", val);

  // CMD16: 设置块长度，参数为块大小
  SD_Send_Command(pSDMMC, CMD16, blkSize);
  val = sdioif->response[0];
  printfPink("response: %p\n", val);

  pSDMMC->BLKSIZ = 512; // 设置控制器块大小寄存器
  return 0;
}

/**
 * @brief 设置SDIO回调函数
 *
 * 配置事件唤醒和等待回调函数指针，用于异步事件处理
 *
 * @param pSDMMC SDMMC控制器指针
 * @param wake_evt 事件唤醒回调函数
 * @param wait_evt 事件等待回调函数
 */
void SDIO_Setup_Callback(SDMMC_T *pSDMMC,
                         void (*wake_evt)(SDMMC_T *pSDMMC, uint32 event,
                                          uint32 arg),
                         uint32 (*wait_evt)(SDMMC_T *pSDMMC, uint32 event,
                                            uint32 arg))
{
  sdioif->wake_evt = wake_evt;
  sdioif->wait_evt = wait_evt;
}

/**
 * @brief 向SD卡写入数据
 *
 * 使用CMD24(单块写入)向指定扇区写入数据：
 * 1. 计算需要写入的块数(每块512字节)
 * 2. 对每个块：设置块大小和字节计数，发送CMD24
 * 3. 轮询TXDR中断(bit4)，向FIFO写入数据
 * 4. 处理中断，等待写入完成
 *
 * @param dat 要写入的数据指针
 * @param size 数据大小(以32位字为单位)
 * @param addr 起始扇区地址
 * @return 始终返回0
 */
uint32 sd_write(uint32 *dat, int size, int addr)
{
  int blk;
  int tt = 0;
  // 计算需要的块数，每块512字节=128个32位字
  if ((size * 4) % 512)
  {
    blk = size * 4 / 512 + 1; // 不足一块的向上取整
  }
  else
  {
    blk = size * 4 / 512; // 恰好整数块
  }
  // printf("size %d, blk:%d\n", size, blk);
  for (int i = 0; i < blk; i++)
  { // 逐块写入
    tt = 0;
    [[maybe_unused]] int ss = 1;
    SDMMC->BLKSIZ = 512;                     // 设置块大小为512字节
    SDMMC->BYTCNT = 512;                     // 设置传输字节数为512
    SD_Send_Command(SDMMC, CMD24, addr + i); // CMD24: 单块写入，参数为扇区地址
    while (SDMMC->RINTSTS & 0x10)
    { // 轮询TXDR中断(bit4)，表示需要发送数据
      if (tt < size)
      {
        *(volatile uint32 *)(SD_BASE_V + 0x200) = dat[tt]; // 向FIFO(偏移0x200)写入数据
      }
      else
        *(volatile uint32 *)(SD_BASE_V + 0x200) = 0; // 不足部分填0
      tt++;
      // printf("rintst: %p\n", LPC_SDMMC->RINTSTS);
      // printf("data %d: %d\n", i, temp_data);
      for (int j = 0; j < 100000; j++)
      { // 延时等待
        /* code */
      }
      SD_IRQHandler(SDMMC); // 处理中断
    }
    // printf("tt: %d\n", tt);
    // tt = 10;
    // ss = tt;
  }

  return 0;
}

/**
 * @brief 从SD卡读取数据
 *
 * 使用CMD17(单块读取)从指定扇区读取数据：
 * 1. 计算需要读取的块数(每块512字节)
 * 2. 对每个块：设置块大小和字节计数，发送CMD17
 * 3. 从FIFO读取数据(每块128个32位字)
 *
 * @param dat 存储读取数据的缓冲区指针
 * @param size 要读取的数据大小(以32位字为单位)
 * @param addr 起始扇区地址
 * @return 始终返回0
 */
uint32 sd_read(uint32 *dat, int size, int addr)
{
  int blk;

  // 计算需要的块数，每块512字节=128个32位字
  if ((size * 4) % 512)
  {
    blk = size * 4 / 512 + 1; // 不足一块的向上取整
  }
  else
  {
    blk = size * 4 / 512; // 恰好整数块
  }
  int tt = 0;

  for (int i = 0; i < blk; i++)
  {                                          // 逐块读取
    SDMMC->BLKSIZ = 512;                     // 设置块大小为512字节
    SDMMC->BYTCNT = 512;                     // 设置传输字节数为512
    SD_Send_Command(SDMMC, CMD17, addr + i); // CMD17: 单块读取，参数为扇区地址
    // while (SDMMC->RINTSTS & 0x20)  // 可选：轮询RXDR中断(bit5)方式读取
    // {
    // 	if (tt < size)
    // 	{
    // 		dat[tt] = *(uint32 *)(SD_BASE_V + 0x200);
    // 	}
    // 	temp = *(uint32 *)(SD_BASE_V + 0x200);
    // 	tt++;
    // 	// printf("rintst: %p\n", LPC_SDMMC->RINTSTS);
    // 	// printf("data %d: %d\n", i, temp_data);
    // 	for (int j = 0; j < 100000; j++)
    // 	{
    // 		/* code */
    // 	}
    // 	SD_IRQHandler(SDMMC);
    // }
    // 当前使用固定次数读取方式(每块128个32位字)
    for (int j = 0; j < 128; j++)
    {
      dat[tt] = *(volatile uint32 *)(SD_BASE_V + 0x200); // 从FIFO(偏移0x200)读取数据
      printf("data: %d: 0x%p\n", tt, dat[tt]);
      tt++;
      // printf("rintst: %p\n", SDMMC->RINTSTS);
      // printf("data %d: %d\n",tt, dat[tt]);
      for (int j = 0; j < 100000; j++)
      { // 延时等待
        /* code */
      }
    }
    // SD_IRQHandler(SDMMC);
  }
  // printf("tt: %d\n", tt);
  return 0;
}

void sd_init()
{

  uint32 rca;
  uint16 fifo_depth;

  platform_init(SDMMC);

  while (Platform_CardNDetect(SDMMC))
  {
  }
  printfPink("HCON: %p\n", SDMMC->HCON);
  SDIO_Setup_Callback(SDMMC, SDIO_WakeEvent, SDIO_WaitEvent);

  rca = SD_Card_Init(SDMMC, 400000);

  SD_Card_SetBlockSize(SDMMC, 512, rca);

  printfPink("FIFOTH: %p\n", SDMMC->FIFOTH);

  uint32 fifoth_t = SDMMC->FIFOTH;

  fifo_depth = ((fifoth_t & 0x0fff0000) >> 16) + 1;

  SDMMC->FIFOTH = ((fifoth_t & 0xf0000000) | ((fifo_depth / 2 - 1) << 16) |
                   (fifo_depth / 2));

  printfPink("FIFOTH: %p\n", SDMMC->FIFOTH);

  printfPink("HCON: %p\n", SDMMC->HCON);

  /* Enable the SDIO Card Interrupt */
  // if (!SDIO_Card_EnableInt(LPC_SDMMC, 1)) {
  // 	printf("DBG: Enabled interrupt for function 1\r\n");
  // }

  printfPink("Card interface enabled use AT commands!\r\n");

#ifdef debug
  printfPink("sd_init\n");
#endif
}

int sd_test(void)
{
  uint32 rca;
  uint16 fifo_depth;

  platform_init(SDMMC);

  while (Platform_CardNDetect(SDMMC))
  {
  }

  SDIO_Setup_Callback(SDMMC, SDIO_WakeEvent, SDIO_WaitEvent);

  rca = SD_Card_Init(SDMMC, 400000);

  SD_Card_SetBlockSize(SDMMC, 512, rca);

  printf("FIFOTH: %p\n", SDMMC->FIFOTH);

  uint32 fifoth_t = SDMMC->FIFOTH;

  fifo_depth = ((fifoth_t & 0x0fff0000) >> 16) + 1;

  SDMMC->FIFOTH = ((fifoth_t & 0xf0000000) | ((fifo_depth / 2 - 1) << 16) |
                   (fifo_depth / 2));

  printf("FIFOTH: %p\n", SDMMC->FIFOTH);

  printf("HCON: %p\n", SDMMC->HCON);

  /* Enable the SDIO Card Interrupt */
  // if (!SDIO_Card_EnableInt(LPC_SDMMC, 1)) {
  // 	printf("DBG: Enabled interrupt for function 1\r\n");
  // }

  printf("Card interface enabled use AT commands!\r\n");

  SDMMC->BLKSIZ = 512;
  SDMMC->BYTCNT = 512;

  SD_Send_Command(SDMMC, CMD24, 1);
  printf("response: %p\n", SDMMC->RESP0);

  int tt = 1;
  while (SDMMC->RINTSTS & 0x10)
  {
    *(volatile uint32 *)(SD_BASE_V + 0x200) = tt;
    tt++;
    // printf("rintst: %p\n", LPC_SDMMC->RINTSTS);
    // printf("data %d: %d\n", i, temp_data);
    for (int j = 0; j < 100000; j++)
    {
      /* code */
    }
    SD_IRQHandler(SDMMC);
  }
  // printf("tt: %d\n", tt);

  printf("rintst: %p\n", SDMMC->RINTSTS);
  SDMMC->BLKSIZ = 512;
  SDMMC->BYTCNT = 512;
  SD_Send_Command(SDMMC, CMD17, 1);
  printf("response: %p\n", SDMMC->RESP0);
  uint32 temp_data;
  for (int i = 0; i < 128; i++)
  {
    // wait_for_read_irq(LPC_SDMMC);
    temp_data = *(volatile uint32 *)(SD_BASE_V + 0x200);
    printf("rintst: %p\n", SDMMC->RINTSTS);
    printf("data %d: %d\n", i, temp_data);
    for (int j = 0; j < 100000; j++)
    {
      /* code */
    }
  }
  // tt = 0;
  // while (SDMMC->RINTSTS & 0x20)
  // 	{
  // 		temp_data = *(uint32 *)(SD_BASE_V + 0x200);
  // 		tt++;
  // 		printf("rintst: %p\n", SDMMC->RINTSTS);
  // 		printf("data %d: %d\n", tt, temp_data);
  // 		for (int j = 0; j < 100000; j++)
  // 		{
  // 			/* code */
  // 		}
  // 		// SD_IRQHandler(SDMMC);
  // 	}

  return 0;
}

void test_sdcard(void)
{
  int bsize = 512;
  uint32 buf[bsize];

  for (int sec = 1; sec < 6; sec++)
  {
    for (int i = 0; i < bsize; i++)
    {
      buf[i] = 0xaa; // data to be written
    }

    sd_write((uint32 *)buf, bsize, sec);

    for (int i = 0; i < bsize; i++)
    {
      buf[i] = 0xff; // fill in junk
    }

    sd_read((uint32 *)buf, bsize, sec);
    for (int i = 0; i < bsize; i++)
    {
      if (buf[i] == 0xaa)
      {
        printf("read back ok ");
      }
      else
      {
        printf("read back error ");
      }
      if (0 == i % 16)
      {
        printf("\n");
      }

      printf("%x ", buf[i]);
    }
    printf("\n");
  }

  while (1)
    ;
}