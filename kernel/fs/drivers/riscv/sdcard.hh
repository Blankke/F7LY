#include "types.hh"
#include "memlayout.hh"

typedef struct {				/*!< SDMMC Structure        */
	uint32  CTRL;		/*!< Control Register       */
	uint32  PWREN;		/*!< Power Enable Register  */
	uint32  CLKDIV;		/*!< Clock Divider Register */
	uint32  CLKSRC;		/*!< SD Clock Source Register */
	uint32  CLKENA;		/*!< Clock Enable Register  */
	uint32  TMOUT;		/*!< Timeout Register       */
	uint32  CTYPE;		/*!< Card Type Register     */
	uint32  BLKSIZ;		/*!< Block Size Register    */
	uint32  BYTCNT;		/*!< Byte Count Register    */
	uint32  INTMASK;		/*!< Interrupt Mask Register */
	uint32  CMDARG;		/*!< Command Argument Register */
	uint32  CMD;			/*!< Command Register       */
	uint32  RESP0;		/*!< Response Register 0    */
	uint32  RESP1;		/*!< Response Register 1    */
	uint32  RESP2;		/*!< Response Register 2    */
	uint32  RESP3;		/*!< Response Register 3    */
	uint32  MINTSTS;		/*!< Masked Interrupt Status Register */
	uint32  RINTSTS;		/*!< Raw Interrupt Status Register */
	uint32  STATUS;		/*!< Status Register        */
	uint32  FIFOTH;		/*!< FIFO Threshold Watermark Register */
	uint32  CDETECT;		/*!< Card Detect Register   */
	uint32  WRTPRT;		/*!< Write Protect Register */
	uint32  RESERVED0;		/*!< General Purpose Input/Output Register */   //gpio
	uint32  TCBCNT;		/*!< Transferred CIU Card Byte Count Register */
	uint32  TBBCNT;		/*!< Transferred Host to BIU-FIFO Byte Count Register */
	uint32  DEBNCE;		/*!< Debounce Count Register */
	uint32  USRID;		/*!< User ID Register       */
	uint32  VERID;		/*!< Version ID Register    */
	uint32  HCON;
	uint32  UHS_REG;		/*!< UHS-1 Register         */
	uint32  RST_N;		/*!< Hardware Reset         */
	uint32  RESERVED1;
	uint32  BMOD;		/*!< Bus Mode Register      */
	uint32  PLDMND;		/*!< Poll Demand Register   */
	uint32  DBADDR;		/*!< Descriptor List Base Address Register */
	uint32  IDSTS;		/*!< Internal DMAC Status Register */
	uint32  IDINTEN;		/*!< Internal DMAC Interrupt Enable Register */
	uint32  DSCADDR;		/*!< Current Host Descriptor Address Register */
	uint32  BUFADDR;		/*!< Current Buffer Descriptor Address Register */
} SDMMC_T;


#define SDMMC                 ((SDMMC_T            *) SD_BASE_V)

/** @brief  SDIO DMA descriptor control (des0) register defines
 */
#define MCI_DMADES0_OWN         (1UL << 31)		/*!< DMA owns descriptor bit */
#define MCI_DMADES0_CES         (1 << 30)		/*!< Card Error Summary bit */
#define MCI_DMADES0_ER          (1 << 5)		/*!< End of descriptopr ring bit */
#define MCI_DMADES0_CH          (1 << 4)		/*!< Second address chained bit */
#define MCI_DMADES0_FS          (1 << 3)		/*!< First descriptor bit */
#define MCI_DMADES0_LD          (1 << 2)		/*!< Last descriptor bit */
#define MCI_DMADES0_DIC         (1 << 1)		/*!< Disable interrupt on completion bit */

/** @brief  SDIO DMA descriptor size (des1) register defines
 */
#define MCI_DMADES1_BS1(x)      (x)				/*!< Size of buffer 1 */
#define MCI_DMADES1_BS2(x)      ((x) << 13)		/*!< Size of buffer 2 */
#define MCI_DMADES1_MAXTR       4096			/*!< Max transfer size per buffer */

/** @brief  SDIO control register defines
 */
#define MCI_CTRL_USE_INT_DMAC   (1 << 25)		/*!< Use internal DMA */
#define MCI_CTRL_CARDV_MASK     (0x7 << 16)		/*!< SD_VOLT[2:0} pins output state mask */
#define MCI_CTRL_CEATA_INT_EN   (1 << 11)		/*!< Enable CE-ATA interrupts */
#define MCI_CTRL_SEND_AS_CCSD   (1 << 10)		/*!< Send auto-stop */
#define MCI_CTRL_SEND_CCSD      (1 << 9)		/*!< Send CCSD */
#define MCI_CTRL_ABRT_READ_DATA (1 << 8)		/*!< Abort read data */
#define MCI_CTRL_SEND_IRQ_RESP  (1 << 7)		/*!< Send auto-IRQ response */
#define MCI_CTRL_READ_WAIT      (1 << 6)		/*!< Assert read-wait for SDIO */
#define MCI_CTRL_INT_ENABLE     (1 << 4)		/*!< Global interrupt enable */
#define MCI_CTRL_DMA_RESET      (1 << 2)		/*!< Reset internal DMA */
#define MCI_CTRL_FIFO_RESET     (1 << 1)		/*!< Reset data FIFO pointers */
#define MCI_CTRL_RESET          (1 << 0)		/*!< Reset controller */

/** @brief SDIO Power Enable register defines
 */
#define MCI_POWER_ENABLE        0x1				/*!< Enable slot power signal (SD_POW) */

/** @brief SDIO Clock divider register defines
 */
#define MCI_CLOCK_DIVIDER(dn, d2) ((d2) << ((dn) * 8))	/*!< Set cklock divider */

/** @brief SDIO Clock source register defines
 */
#define MCI_CLKSRC_CLKDIV0      0
#define MCI_CLKSRC_CLKDIV1      1
#define MCI_CLKSRC_CLKDIV2      2
#define MCI_CLKSRC_CLKDIV3      3
#define MCI_CLK_SOURCE(clksrc)  (clksrc)		/*!< Set cklock divider source */

/** @brief SDIO Clock Enable register defines
 */
#define MCI_CLKEN_LOW_PWR       (1 << 16)		/*!< Enable clock idle for slot */
#define MCI_CLKEN_ENABLE        (1 << 0)		/*!< Enable slot clock */

/** @brief SDIO time-out register defines
 */
#define MCI_TMOUT_DATA(clks)    ((clks) << 8)	/*!< Data timeout clocks */
#define MCI_TMOUT_DATA_MSK      0xFFFFFF00
#define MCI_TMOUT_RESP(clks)    ((clks) & 0xFF)	/*!< Response timeout clocks */
#define MCI_TMOUT_RESP_MSK      0xFF

/** @brief SDIO card-type register defines
 */
#define MCI_CTYPE_8BIT          (1 << 16)		/*!< Enable 4-bit mode */
#define MCI_CTYPE_4BIT          (1 << 0)		/*!< Enable 8-bit mode */

/** @brief SDIO Interrupt status & mask register defines
 */
#define MCI_INT_SDIO            (1 << 16)		/*!< SDIO interrupt */
#define MCI_INT_EBE             (1 << 15)		/*!< End-bit error */
#define MCI_INT_ACD             (1 << 14)		/*!< Auto command done */
#define MCI_INT_SBE             (1 << 13)		/*!< Start bit error */
#define MCI_INT_HLE             (1 << 12)		/*!< Hardware locked error */
#define MCI_INT_FRUN            (1 << 11)		/*!< FIFO overrun/underrun error */
#define MCI_INT_HTO             (1 << 10)		/*!< Host data starvation error */
#define MCI_INT_DTO             (1 << 9)		/*!< Data timeout error */
#define MCI_INT_RTO             (1 << 8)		/*!< Response timeout error */
#define MCI_INT_DCRC            (1 << 7)		/*!< Data CRC error */
#define MCI_INT_RCRC            (1 << 6)		/*!< Response CRC error */
#define MCI_INT_RXDR            (1 << 5)		/*!< RX data ready */
#define MCI_INT_TXDR            (1 << 4)		/*!< TX data needed */
#define MCI_INT_DATA_OVER       (1 << 3)		/*!< Data transfer over */
#define MCI_INT_CMD_DONE        (1 << 2)		/*!< Command done */
#define MCI_INT_RESP_ERR        (1 << 1)		/*!< Command response error */
#define MCI_INT_CD              (1 << 0)		/*!< Card detect */

/** @brief SDIO Command register defines
 */
#define MCI_CMD_START           (1UL << 31)		/*!< Start command */
#define MCI_CMD_VOLT_SWITCH     (1 << 28)		/*!< Voltage switch bit */
#define MCI_CMD_BOOT_MODE       (1 << 27)		/*!< Boot mode */
#define MCI_CMD_DISABLE_BOOT    (1 << 26)		/*!< Disable boot */
#define MCI_CMD_EXPECT_BOOT_ACK (1 << 25)		/*!< Expect boot ack */
#define MCI_CMD_ENABLE_BOOT     (1 << 24)		/*!< Enable boot */
#define MCI_CMD_CCS_EXP         (1 << 23)		/*!< CCS expected */
#define MCI_CMD_CEATA_RD        (1 << 22)		/*!< CE-ATA read in progress */
#define MCI_CMD_UPD_CLK         (1 << 21)		/*!< Update clock register only */
#define MCI_CMD_INIT            (1 << 15)		/*!< Send init sequence */
#define MCI_CMD_STOP            (1 << 14)		/*!< Stop/abort command */
#define MCI_CMD_PRV_DAT_WAIT    (1 << 13)		/*!< Wait before send */
#define MCI_CMD_SEND_STOP       (1 << 12)		/*!< Send auto-stop */
#define MCI_CMD_STRM_MODE       (1 << 11)		/*!< Stream transfer mode */
#define MCI_CMD_DAT_WR          (1 << 10)		/*!< Read(0)/Write(1) selection */
#define MCI_CMD_DAT_EXP         (1 << 9)		/*!< Data expected */
#define MCI_CMD_RESP_CRC        (1 << 8)		/*!< Check response CRC */
#define MCI_CMD_RESP_LONG       (1 << 7)		/*!< Response length */
#define MCI_CMD_RESP_EXP        (1 << 6)		/*!< Response expected */
#define MCI_CMD_INDX(n)         ((n) & 0x1F)

/** @brief SDIO status register definess
 */
#define MCI_STS_GET_FCNT(x)     (((x) >> 17) & 0x1FF)

/** @brief SDIO FIFO threshold defines
 */
#define MCI_FIFOTH_TX_WM(x)     ((x) & 0xFFF)
#define MCI_FIFOTH_RX_WM(x)     (((x) & 0xFFF) << 16)
#define MCI_FIFOTH_DMA_MTS_1    (0UL << 28)
#define MCI_FIFOTH_DMA_MTS_4    (1UL << 28)
#define MCI_FIFOTH_DMA_MTS_8    (2UL << 28)
#define MCI_FIFOTH_DMA_MTS_16   (3UL << 28)
#define MCI_FIFOTH_DMA_MTS_32   (4UL << 28)
#define MCI_FIFOTH_DMA_MTS_64   (5UL << 28)
#define MCI_FIFOTH_DMA_MTS_128  (6UL << 28)
#define MCI_FIFOTH_DMA_MTS_256  (7UL << 28)

/** @brief Bus mode register defines
 */
#define MCI_BMOD_PBL1           (0 << 8)		/*!< Burst length = 1 */
#define MCI_BMOD_PBL4           (1 << 8)		/*!< Burst length = 4 */
#define MCI_BMOD_PBL8           (2 << 8)		/*!< Burst length = 8 */
#define MCI_BMOD_PBL16          (3 << 8)		/*!< Burst length = 16 */
#define MCI_BMOD_PBL32          (4 << 8)		/*!< Burst length = 32 */
#define MCI_BMOD_PBL64          (5 << 8)		/*!< Burst length = 64 */
#define MCI_BMOD_PBL128         (6 << 8)		/*!< Burst length = 128 */
#define MCI_BMOD_PBL256         (7 << 8)		/*!< Burst length = 256 */
#define MCI_BMOD_DE             (1 << 7)		/*!< Enable internal DMAC */
#define MCI_BMOD_DSL(len)       ((len) << 2)	/*!< Descriptor skip length */
#define MCI_BMOD_FB             (1 << 1)		/*!< Fixed bursts */
#define MCI_BMOD_SWR            (1 << 0)		/*!< Software reset of internal registers */

/** @brief SDIO中断掩码定义 - 控制哪些中断被使能 */
#define SDIO_CMD_INT_MSK    0xA146       /*!< 命令相关中断掩码：命令完成、超时、CRC错误等 */
#define SDIO_DATA_INT_MSK   0xBE88       /*!< 数据传输中断掩码：数据完成、FIFO就绪、传输错误等 */
#define SDIO_CARD_INT_MSK   (1UL << 16)  /*!< SDIO卡中断掩码：来自SDIO功能的中断 */

/** @brief SDIO命令选项标志 - 用于配置命令属性 */
#define SD_WAIT_PRE			 (1UL << 13)  /*!< 等待前一个数据传输完成 */
#define SDIO_CMD_CRC         (1UL << 8)   /*!< 响应必须有有效的CRC校验 */
#define SDIO_CMD_DATA        (1UL << 9)   /*!< 命令是数据传输命令 */
#define SD_CMD_WRITE		(1L << 10)    /*!< 数据传输方向：写(1)或读(0) */

/** @brief SDIO命令响应类型定义 - 根据SD规范定义的响应格式 */
#define SDIO_CMD_RESP_R1     (1UL << 6)   /*!< R1响应：48位，包含卡状态 */
#define SDIO_CMD_RESP_R2     (3UL << 6)   /*!< R2响应：136位，包含CID或CSD */
#define SDIO_CMD_RESP_R3     (1UL << 6)   /*!< R3响应：48位，包含OCR */
#define SDIO_CMD_RESP_R4     (1UL << 6)   /*!< R4响应：48位，SDIO专用 */
#define SDIO_CMD_RESP_R5     (1UL << 6)   /*!< R5响应：48位，SDIO专用 */
#define SDIO_CMD_RESP_R6     (1UL << 6)   /*!< R6响应：48位，包含RCA */

/** @brief SD卡命令定义 - 根据SD规范定义的标准命令 */

// 基础命令 - 用于卡识别和初始化
#define CMD0            (0 | (1 << 15))         /*!< GO_IDLE_STATE: 重置SD卡到idle状态 */
#define CMD2			(2 | SDIO_CMD_RESP_R2)   /*!< ALL_SEND_CID: 所有卡发送CID */
#define CMD3			(3 | SDIO_CMD_RESP_R6)   /*!< SEND_RELATIVE_ADDR: 要求卡发布RCA */
#define CMD5            (5 | SDIO_CMD_RESP_R4)   /*!< IO_SEND_OP_COND: SDIO设备检测 */
#define CMD8			(8 | (1UL << 6))         /*!< SEND_IF_COND: 检测SD2.0设备和电压 */
#define CMD7            (7 | SDIO_CMD_RESP_R1)   /*!< SELECT_CARD: 选择/取消选择卡 */
#define CMD16           (16 | SDIO_CMD_RESP_R1)  /*!< SET_BLOCKLEN: 设置块长度 */

// 数据传输命令
#define CMD17           (17 | SDIO_CMD_RESP_R1 | SDIO_CMD_DATA | SD_WAIT_PRE)  /*!< READ_SINGLE_BLOCK: 单块读取 */
#define CMD24           (24 | SDIO_CMD_RESP_R1 | SDIO_CMD_DATA | SD_CMD_WRITE | SD_WAIT_PRE)  /*!< WRITE_BLOCK: 单块写入 */

// SDIO特定命令
#define CMD52           (52 | SDIO_CMD_RESP_R5 | SDIO_CMD_CRC)    /*!< IO_RW_DIRECT: SDIO直接读写 */
#define CMD53           (53 | SDIO_CMD_RESP_R5 | SDIO_CMD_DATA | SDIO_CMD_CRC)  /*!< IO_RW_EXTENDED: SDIO扩展读写 */

// 应用专用命令(需要先发送CMD55)
#define CMD55			(55 | (1UL << 6))        /*!< APP_CMD: 下一个命令是应用专用命令 */
#define ACMD41			(41 | (1UL << 6))        /*!< SD_SEND_OP_COND: SD卡初始化命令 */

/**
 * @brief SDIO事件类型枚举
 * 
 * 定义SDIO驱动中使用的各种事件类型，用于状态机管理和事件通知
 */
enum SDIO_EVENT
{
	SDIO_START_COMMAND,  /*!< SDIO驱动即将开始命令传输 */
	SDIO_START_DATA,     /*!< SDIO驱动即将开始数据传输 */
	SDIO_WAIT_DELAY,     /*!< SDIO驱动需要等待指定的毫秒数 */
	SDIO_WAIT_COMMAND,   /*!< SDIO驱动正在等待命令完成 */
	SDIO_WAIT_DATA,      /*!< SDIO驱动正在等待数据传输完成 */

	SDIO_CARD_DETECT,    /*!< SDIO驱动检测到卡插入/拔出 */
	SDIO_CMD_ERR,        /*!< 命令传输错误 */
	SDIO_CMD_DONE,       /*!< 命令传输成功完成 */
	SDIO_DATA_ERR,       /*!< 数据传输错误 */
	SDIO_DATA_DONE,      /*!< 数据传输成功完成 */
	SDIO_CARD_INT,       /*!< SDIO卡中断(来自功能) */
};

int sd_test(void);
int platform_init(SDMMC_T *pSDMMC);
int Platform_CardNDetect(SDMMC_T *pSDMMC);
uint32 SD_Card_Init(SDMMC_T *pSDMMC, uint32 freq);
void SD_SetIntMask(SDMMC_T *pSDMMC, uint32 iVal);
int SD_SendCmd(SDMMC_T *pSDMMC, uint32 cmd, uint32 arg);
uint32 wait_for_sdio_irq(SDMMC_T *pSDMMC);
uint32 SD_Send_Command(SDMMC_T *pSDMMC, uint32 cmd, uint32 arg);
int SD_Card_SetBlockSize(SDMMC_T *pSDMMC, uint32 blkSize, uint32 rca);
void SD_IRQHandler(SDMMC_T *pSDMMC);
void SD_GetResponse(SDMMC_T *pSDMMC, uint32 *resp);
void SD_SetCardType(SDMMC_T *pSDMMC, uint32 ctype);
void SD_SetClock(SDMMC_T *pSDMMC, uint32 clk_rate, uint32 speed);
void SDIO_Setup_Callback(SDMMC_T *pSDMMC,
	void (*wake_evt)(SDMMC_T *pSDMMC, uint32 event, uint32 arg),
	uint32 (*wait_evt)(SDMMC_T *pSDMMC, uint32 event, uint32 arg));
void sd_init();
uint32 sd_read(uint32 *dat, int size, int addr);
uint32 sd_write(uint32 *dat, int size, int addr);
void test_sdcard();