#include "fs/drivers/riscv/sdcard.hh"
#include "printer.hh"
#include "types.hh"

static struct sdio_context_t {
  void (*wake_evt)(SDMMC_T *pSDMMC, uint32 event, uint32 arg);
  uint32 (*wait_evt)(SDMMC_T *pSDMMC, uint32 event, uint32 arg);
  uint32 flag;
  uint32 response[4];
  int fnum;
  uint16 blkSz[8]; /* Block size setting for the 8- function blocks */
} sdio_context;
static struct sdio_context_t *sdioif = &sdio_context;

enum SDIO_STATE {
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
static struct sdio_state {
  enum SDIO_STATE cstate;
  enum SDIO_STATE dstate;
  uint32 carg;
  uint32 darg;
} sstate;

static volatile uint32 card_int;

static uint32 SDIO_WaitEvent(SDMMC_T *pSDMMC, uint32 event, uint32 arg) {
  uint32 ret = 0;
  // printf("%d", arg);
  switch (event) {
  case SDIO_START_COMMAND:
    sstate.cstate = SDIO_STATE_CMD_WAIT;
    break;

  case SDIO_START_DATA:
    sstate.dstate = SDIO_STATE_DATA_WAIT;
    break;

  case SDIO_WAIT_COMMAND:
    while (*((volatile enum SDIO_STATE *)&sstate.cstate) ==
           SDIO_STATE_CMD_WAIT) {
      // __WFI();
      wait_for_sdio_irq(pSDMMC);
      printfPink("WFI\n");
    }
    ret = sstate.carg;
    break;

  case SDIO_WAIT_DATA:
    while (*((volatile enum SDIO_STATE *)&sstate.dstate) ==
           SDIO_STATE_DATA_WAIT) {
      // __WFI();
      wait_for_sdio_irq(pSDMMC);
      printfPink("WFI\n");
    }
    ret = sstate.darg;
    break;

  case SDIO_WAIT_DELAY: {
    // uint32 cntr = ms_cnt + arg;
    // while (cntr > ms_cnt) {
    // 	printf("cntr: %d ms_cnt: %d", cntr, ms_cnt);
    // }
    while (arg) {
      arg--;
    }

    break;
  }
  default:
    break;
  }
  return ret;
}

static void SDIO_WakeEvent(SDMMC_T *pSDMMC, uint32 event, uint32 arg) {
  switch (event) {
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

int platform_init(SDMMC_T *pSDMMC) {
  /* Enable SDIO module clock */
  // Chip_Clock_EnableOpts(CLK_MX_SDIO, true, true, 1);

  /* Software reset */
  pSDMMC->BMOD = MCI_BMOD_SWR;
  /* reset all blocks */
  pSDMMC->CTRL = MCI_CTRL_RESET | MCI_CTRL_FIFO_RESET | MCI_CTRL_DMA_RESET;
  while (pSDMMC->CTRL &
         (MCI_CTRL_RESET | MCI_CTRL_FIFO_RESET | MCI_CTRL_DMA_RESET)) {
    printfPink("ctrl:%p\n", pSDMMC->CTRL);
  }

  /* Clear the interrupts for the host controller */
  pSDMMC->PWREN = 1;
  pSDMMC->RINTSTS = 0xFFFFFFFF;

  /* Internal DMA setup for control register */
  // pSDMMC->CTRL = MCI_CTRL_USE_INT_DMAC | MCI_CTRL_INT_ENABLE;
  pSDMMC->CTRL = MCI_CTRL_INT_ENABLE;
  pSDMMC->INTMASK = 0;

  /* Put in max timeout */
  pSDMMC->TMOUT = 0xFFFFFFFF;

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

void SD_IRQHandler(SDMMC_T *pSDMMC) {
  uint32 status = pSDMMC->MINTSTS;
  uint32 iclr = 0;

  /* Card Detected */
  if (status & 1) {
    sdioif->wake_evt(pSDMMC, SDIO_CARD_DETECT, 0);
    iclr = 1;
  }

  /* Command event error */
  if (status & (SDIO_CMD_INT_MSK & ~4)) {
    sdioif->wake_evt(pSDMMC, SDIO_CMD_ERR, (status & (SDIO_CMD_INT_MSK & ~4)));
    iclr |= status & SDIO_CMD_INT_MSK;
  } else if (status & 4) {
    /* Command event done */
    sdioif->wake_evt(pSDMMC, SDIO_CMD_DONE, status);
    iclr |= status & SDIO_CMD_INT_MSK;
  }

  /* Command event error */
  if (status & (SDIO_DATA_INT_MSK & ~8)) {
    sdioif->wake_evt(pSDMMC, SDIO_DATA_ERR, status & (SDIO_DATA_INT_MSK & ~8));
    iclr |= (status & SDIO_DATA_INT_MSK) | (3 << 4);
  } else if (status & 8) {
    /* Command event done */
    sdioif->wake_evt(pSDMMC, SDIO_DATA_DONE, status);
    iclr |= (status & SDIO_DATA_INT_MSK) | (3 << 4);
  }

  /* Handle Card interrupt */
  if (status & SDIO_CARD_INT_MSK) {
    sdioif->wake_evt(pSDMMC, SDIO_CARD_INT, 0);
    iclr |= status & SDIO_CARD_INT_MSK;
  }

  /* Clear the interrupts */
  pSDMMC->RINTSTS = iclr;
}

uint32 wait_for_sdio_irq(SDMMC_T *pSDMMC) {
  uint32 rintst;
  while (1) {
    rintst = pSDMMC->RINTSTS;
    // printf("rintst: %p\n", rintst);
    if (rintst & 0xffff0004) {
      break;
    }
  }
  SD_IRQHandler(pSDMMC);
  return 0;
}

uint32 SD_Send_Command(SDMMC_T *pSDMMC, uint32 cmd, uint32 arg) {
  uint32 ret = 0, ival;
  uint32 imsk = pSDMMC->INTMASK;
  // ret = sdioif->wait_evt(pSDMMC, SDIO_START_COMMAND, (cmd & 0x3F));
  // ival = SDIO_CMD_INT_MSK & ~ret;
  ival = SDIO_CMD_INT_MSK;

  /* Set data interrupts for data commands */
  if (cmd & SDIO_CMD_DATA) {
    ival |= SDIO_DATA_INT_MSK;
    imsk |= SDIO_DATA_INT_MSK;
  }

  SD_SetIntMask(pSDMMC, ival);
  SD_SendCmd(pSDMMC, cmd, arg);
  // ret = sdioif->wait_evt(pSDMMC, SDIO_WAIT_COMMAND, 0);
  wait_for_sdio_irq(pSDMMC);
  pSDMMC->RINTSTS = 0xFFFFFFFF; // clear interrupt
  // if (!ret && (cmd & SDIO_CMD_RESP_R1)) {
  // 	Chip_SDIF_GetResponse(pSDMMC, &sdioif->response[0]);
  // }

  SD_GetResponse(pSDMMC, &sdioif->response[0]);
  SD_SetIntMask(pSDMMC, imsk);
  return ret;
}

void SD_GetResponse(SDMMC_T *pSDMMC, uint32 *resp) {
  /* on this chip response is not a fifo so read all 4 regs */
  resp[0] = pSDMMC->RESP0;
  resp[1] = pSDMMC->RESP1;
  resp[2] = pSDMMC->RESP2;
  resp[3] = pSDMMC->RESP3;
}

void SD_SetCardType(SDMMC_T *pSDMMC, uint32 ctype) { pSDMMC->CTYPE = ctype; }

void SD_SetClock(SDMMC_T *pSDMMC, uint32 clk_rate, uint32 speed) {
  /* compute SD/MMC clock dividers */
  uint32 div;

  div = ((clk_rate / speed) + 2) >> 1;

  if ((div == pSDMMC->CLKDIV) && pSDMMC->CLKENA) {
    return; /* Closest speed is already set */
  }
  /* disable clock */
  pSDMMC->CLKENA = 0;

  /* User divider 0 */
  pSDMMC->CLKSRC = MCI_CLKSRC_CLKDIV0;

  /* inform CIU */
  SD_SendCmd(pSDMMC, MCI_CMD_UPD_CLK | MCI_CMD_PRV_DAT_WAIT, 0);

  /* set divider 0 to desired value */
  pSDMMC->CLKDIV = MCI_CLOCK_DIVIDER(0, div);

  /* inform CIU */
  SD_SendCmd(pSDMMC, MCI_CMD_UPD_CLK | MCI_CMD_PRV_DAT_WAIT, 0);

  /* enable clock */
  pSDMMC->CLKENA = MCI_CLKEN_ENABLE;

  /* inform CIU */
  SD_SendCmd(pSDMMC, MCI_CMD_UPD_CLK | MCI_CMD_PRV_DAT_WAIT, 0);
}

void SD_SetIntMask(SDMMC_T *pSDMMC, uint32 iVal) { pSDMMC->INTMASK = iVal; }

int SD_SendCmd(SDMMC_T *pSDMMC, uint32 cmd, uint32 arg) {
  volatile int tmo = 50;
  volatile int delay;

  /* set command arg reg*/
  pSDMMC->CMDARG = arg;
  pSDMMC->CMD = MCI_CMD_START | cmd;

  /* poll untill command is accepted by the CIU */
  while (--tmo && (pSDMMC->CMD & MCI_CMD_START)) {
    if (tmo & 1) {
      delay = 50;
    } else {
      delay = 18000;
    }

    while (--delay > 1) {
    }
  }

  return (tmo < 1) ? 1 : 0;
}

/*
        Initialize sd card
        return value of rca
        by comedymaker
*/
uint32 SD_Card_Init(SDMMC_T *pSDMMC, uint32 freq) {
  uint32 val;
  uint32 rca;
  /* Set Clock to 400KHz */
  SD_SetCardType(pSDMMC, 0);
  SD_SetClock(pSDMMC, 200000000, freq);
  printfPink("arrive a0\n");
  sdioif->wait_evt(pSDMMC, SDIO_WAIT_DELAY, 100); /* Wait for card to wake up */

  printfPink("arrive a\n");
  printfPink("RINTSTS: %p\n", pSDMMC->RINTSTS);
  printfPink("responese: %p\n", pSDMMC->RESP0);
  SD_Send_Command(pSDMMC, CMD5, 0);
  // if (ret) return ret;
  val = sdioif->response[0];
  printfPink("response: %p\n", val);
  printfPink("arrive b\n");

  printfPink("RINTSTS: %p\n", pSDMMC->RINTSTS);
  SD_Send_Command(pSDMMC, CMD0, 0);
  // if (ret) return ret;
  val = sdioif->response[0];
  printfPink("response: %p\n", val);

  printfPink("RINTSTS: %p\n", pSDMMC->RINTSTS);

  SD_Send_Command(pSDMMC, CMD8, 0x1aa);
  val = sdioif->response[0];
  printfPink("response: %p\n", val);
  printfPink("arrive c\n");

  do {
    SD_Send_Command(pSDMMC, CMD55, 0);
    SD_Send_Command(pSDMMC, ACMD41, 0x40100000);
    val = sdioif->response[0];
  } while ((val & 0x80000000) == 0);

  printfPink("response: %p\n", val);
  printfPink("arrive d\n");

  SD_Send_Command(pSDMMC, CMD2, 0);
  printfPink("response3: %p\n", sdioif->response[3]);
  printfPink("response3: %p\n", sdioif->response[2]);
  printfPink("response3: %p\n", sdioif->response[1]);
  printfPink("response3: %p\n", sdioif->response[0]);
  printfPink("arrive e\n");

  SD_Send_Command(pSDMMC, CMD3, 0);
  val = sdioif->response[0];
  printfPink("response: %p\n", val);
  printfPink("arrive e\n");

  rca = (val & 0xffff0000);
  printfPink("rca: %p\n", rca);

  SD_SetClock(pSDMMC, 200000000, 25000000);
  return rca;
}

int SD_Card_SetBlockSize(SDMMC_T *pSDMMC, uint32 blkSize, uint32 rca) {
  uint32 val;
  SD_Send_Command(pSDMMC, CMD7, rca);
  val = sdioif->response[0];
  printfPink("response: %p\n", val);

  SD_Send_Command(pSDMMC, CMD16, blkSize);
  val = sdioif->response[0];
  printfPink("response: %p\n", val);

  pSDMMC->BLKSIZ = 512;
  return 0;
}

void SDIO_Setup_Callback(SDMMC_T *pSDMMC,
                         void (*wake_evt)(SDMMC_T *pSDMMC, uint32 event,
                                          uint32 arg),
                         uint32 (*wait_evt)(SDMMC_T *pSDMMC, uint32 event,
                                            uint32 arg)) {
  sdioif->wake_evt = wake_evt;
  sdioif->wait_evt = wait_evt;
}

// use cmd24, later can change to cmd25
uint32 sd_write(uint32 *dat, int size, int addr) {
  int blk;
  int tt = 0;
  if ((size * 4) % 512) {
    blk = size * 4 / 512 + 1;
  } else {
    blk = size * 4 / 512;
  }
  // printf("size %d, blk:%d\n", size, blk);
  for (int i = 0; i < blk; i++) {
    tt = 0;
    [[maybe_unused]]int ss = 1;
    SDMMC->BLKSIZ = 512;
    SDMMC->BYTCNT = 512;
    SD_Send_Command(SDMMC, CMD24, addr + i);
    while (SDMMC->RINTSTS & 0x10) {
      if (tt < size) {
        *(uint32 *)(SD_BASE_V + 0x200) = dat[tt];
      } else
        *(uint32 *)(SD_BASE_V + 0x200) = 0;
      tt++;
      // printf("rintst: %p\n", LPC_SDMMC->RINTSTS);
      // printf("data %d: %d\n", i, temp_data);
      for (int j = 0; j < 100000; j++) {
        /* code */
      }
      SD_IRQHandler(SDMMC);
    }
    // printf("tt: %d\n", tt);
    // tt = 10;
    // ss = tt;
  }

  return 0;
}

// use cmd17, later can change to cmd18
uint32 sd_read(uint32 *dat, int size, int addr) {
  int blk;

  if ((size * 4) % 512) {
    blk = size * 4 / 512 + 1;
  } else {
    blk = size * 4 / 512;
  }
  int tt = 0;

  for (int i = 0; i < blk; i++) {
    SDMMC->BLKSIZ = 512;
    SDMMC->BYTCNT = 512;
    SD_Send_Command(SDMMC, CMD17, addr + i);
    // while (SDMMC->RINTSTS & 0x20)
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
    for (int j = 0; j < 128; j++) {

      dat[tt] = *(uint32 *)(SD_BASE_V + 0x200);
      printf("data: %d: %p\n", tt, dat[tt]);
      tt++;
      // printf("rintst: %p\n", SDMMC->RINTSTS);
      // printf("data %d: %d\n",tt, dat[tt]);
      for (int j = 0; j < 100000; j++) {
        /* code */
      }
    }
    // SD_IRQHandler(SDMMC);
  }
  // printf("tt: %d\n", tt);
  return 0;
}

void sd_init() {

  uint32 rca;
  uint16 fifo_depth;

  platform_init(SDMMC);

  while (Platform_CardNDetect(SDMMC)) {
  }

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

int sd_test(void) {
  uint32 rca;
  uint16 fifo_depth;

  platform_init(SDMMC);

  while (Platform_CardNDetect(SDMMC)) {
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
  while (SDMMC->RINTSTS & 0x10) {
    *(uint32 *)(SD_BASE_V + 0x200) = tt;
    tt++;
    // printf("rintst: %p\n", LPC_SDMMC->RINTSTS);
    // printf("data %d: %d\n", i, temp_data);
    for (int j = 0; j < 100000; j++) {
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
  for (int i = 0; i < 128; i++) {
    // wait_for_read_irq(LPC_SDMMC);
    temp_data = *(uint32 *)(SD_BASE_V + 0x200);
    printf("rintst: %p\n", SDMMC->RINTSTS);
    printf("data %d: %d\n", i, temp_data);
    for (int j = 0; j < 100000; j++) {
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

void test_sdcard(void) {
  int bsize = 512;
  uint32 buf[bsize];

  for (int sec = 1; sec < 6; sec++) {
    for (int i = 0; i < bsize; i++) {
      buf[i] = 0xaa; // data to be written
    }

    sd_write((uint32 *)buf, bsize, sec);

    for (int i = 0; i < bsize; i++) {
      buf[i] = 0xff; // fill in junk
    }

    sd_read((uint32 *)buf, bsize, sec);
    for (int i = 0; i < bsize; i++) {
      if (buf[i] == 0xaa) {
        printf("read back ok ");
      } else {
        printf("read back error ");
      }
      if (0 == i % 16) {
        printf("\n");
      }

      printf("%x ", buf[i]);
    }
    printf("\n");
  }

  while (1)
    ;
}