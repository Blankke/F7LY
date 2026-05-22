//
// driver for qemu's virtio disk device.
// uses qemu's mmio interface to virtio.
//
// qemu ... -drive file=fs.img,if=none,format=raw,id=x0 -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
//

#include "types.hh"
#include "platform.hh"

#include "param.h"
#include "mem/memlayout.hh"
#include "spinlock.hh"
#include "sleeplock.hh"
#include "fs/vfs/fs.hh"
#include "fs/buf.hh"
#include "drivers/riscv/virtio2.hh"
#include "libs/string.hh"
#include "virtual_memory_manager.hh"
#include "proc_manager.hh"
#include "tm/timer_manager.hh"
// the address of virtio mmio register r.
#ifdef RISCV
#define R(r) ((volatile uint32 *)(VIRTIO0 + (r)))
#define R2(r) ((volatile uint32 *)(VIRTIO1 + (r)))

namespace
{
  /**
   * @brief mclock 调度概览与扩展建议
   *
   * TODO: 当前 mclock 调度实现与 virtio_disk.c 中的逻辑耦合较紧，建议将调度器提取为独立模块
   * 以便复用与测试，并放置于更通用的位置（例如 fs/drivers/virtio），使其它架构（如 loongarch）也能复用。
   *
   * @details
   * 本文件实现了一个基于服务类（service class）的带宽调度器（mclock），用于在 block IO 层对不同优先级进程
   * 提供带宽保底（reservation）、基于权重的共享（weight）以及可选的限速（limit）。
   * 每个请求入队时计算三类时间标签（微秒单位）：
   * - r_tag (reservation tag): 基于 reservation_bps，用于实现保底带宽；到期的请求优先被服务。
   * - w_tag (weight tag): 基于 class 权重与系统带宽估计（ewma_bps），用于按权重公平共享带宽。
   * - l_tag (limit tag): 基于 limit_bps，如果该标签在未来表示该请求被限流暂缓。
   * 调度顺序：优先选择满足 r_tag 的请求；若无则选择最小 w_tag 的请求；当所有请求被 limit 阻挡时，返回最早的可用时间。
   */

  constexpr int k_mclock_class_count = 8;
    /** @brief 微秒每秒 */
    constexpr uint64 k_usec_per_sec = 1000000ULL;
    /** @brief 表示不限速（带宽值为 0 表示无限制） */
    constexpr uint64 k_unlimited_bps = 0;
    /** @brief EWMA 带宽的默认初始值（64 MiB/s），用于权重时间窗口计算的基线 */
    constexpr uint64 k_default_ewma_bps = 64ULL * 1024ULL * 1024ULL;
    /** @brief 每个服务类的权重（用于权重调度，weight 越大表示该类应该获得越多共享带宽） */
    constexpr uint32 k_class_weight[k_mclock_class_count] = {256, 192, 128, 96, 64, 32, 16, 8};
    /** @brief 每个服务类的带宽保底（字节/秒），用于计算 r_tag 增量，以保证最低带宽 */
  constexpr uint64 k_class_reservation_bps[k_mclock_class_count] = {
      24ULL * 1024ULL * 1024ULL,
      16ULL * 1024ULL * 1024ULL,
      8ULL * 1024ULL * 1024ULL,
      4ULL * 1024ULL * 1024ULL,
      2ULL * 1024ULL * 1024ULL,
      1ULL * 1024ULL * 1024ULL,
      512ULL * 1024ULL,
      256ULL * 1024ULL};
    /** @brief 每个服务类的带宽上限（0 表示无限制） */
    constexpr uint64 k_class_limit_bps[k_mclock_class_count] = {
      k_unlimited_bps, k_unlimited_bps, k_unlimited_bps, k_unlimited_bps,
      k_unlimited_bps, k_unlimited_bps, k_unlimited_bps, k_unlimited_bps};
    /** @brief 权重总和（用于将不同 weight 缩放到统一基准） */
    constexpr uint32 k_total_weight = 792;

  /**
   * @brief 每个服务类维护的队列状态
   */
  struct mclock_queue_state
  {
    struct buf *head;        ///< 队列头，用于维护该服务类的等待 buf 链表
    struct buf *tail;        ///< 队列尾
    uint64 last_r_tag_us;    ///< 最近一次用于 reservation 的 tag 时间（微秒）
    uint64 last_w_tag_us;    ///< 最近一次用于 weight 的 tag 时间（微秒）
    uint64 last_l_tag_us;    ///< 最近一次用于 limit 的 tag 时间（微秒）
  };

  /**
   * @brief 向上取整的 64 位除法
   * @param numerator 分子
   * @param denominator 分母
   * @return 向上取整的商，若 denominator==0 则返回 0
   */
  inline uint64 ceil_div_u64(uint64 numerator, uint64 denominator)
  {
    return denominator == 0 ? 0 : (numerator + denominator - 1) / denominator;
  }

  /**
   * @brief 获取 mclock 使用的当前时间（微秒）
   * @return 当前时间（微秒）
   */
  inline uint64 mclock_now_us()
  {
    tmm::timeval tv = tmm::k_tm.get_time_val();
    return tv.tv_sec * k_usec_per_sec + tv.tv_usec;
  }

  /**
   * @brief 计算传输指定字节数在给定带宽（字节/秒）下所需的微秒数
   *
   * 返回值至少为 1 微秒以避免零时间窗口；对于 bps == 0（无限制）返回 0
   * @param bytes 传输字节数
   * @param bps 带宽（字节/秒），0 表示无限制
   * @return 所需时间（微秒），bps==0 返回 0
   */
  inline uint64 bytes_to_service_us(uint64 bytes, uint64 bps)
  {
    if (bps == 0)
    {
      return 0;
    }
    uint64 usec = ceil_div_u64(bytes * k_usec_per_sec, bps);
    return usec == 0 ? 1 : usec;
  }

  /**
   * @brief 将 bytes 按类权重扩展到统一的权重基准后，基于 ewma_bps 计算服务所需时间
   *
   * 原理：先将 bytes 按比例放大到总权重基准上，再以系统当前带宽估计（ewma_bps）计算时间窗口。
   * @param bytes 请求字节数
   * @param weight 类权重
   * @param ewma_bps 系统带宽估计（字节/秒）
   * @return 所需时间（微秒）
   */
  inline uint64 weighted_service_us(uint64 bytes, uint32 weight, uint64 ewma_bps)
  {
    uint64 scaled_bytes = ceil_div_u64(bytes * k_total_weight, weight == 0 ? 1 : weight);
    return bytes_to_service_us(scaled_bytes, ewma_bps == 0 ? k_default_ewma_bps : ewma_bps);
  }

  /**
   * @brief 将进程的 nice 值映射到服务类索引
   *
   * 先将 nice 值裁剪到 [highest_proc_prio, lowest_proc_prio]，然后按每 5 个 nice 值划分一个服务类
   * @param nice 进程 nice 值
   * @return 服务类索引
   */
  inline int nice_to_service_class(int nice)
  {
    int clamped = nice;
    if (clamped < proc::highest_proc_prio)
    {
      clamped = proc::highest_proc_prio;
    }
    else if (clamped > proc::lowest_proc_prio)
    {
      clamped = proc::lowest_proc_prio;
    }
    return (clamped - proc::highest_proc_prio) / 5;
  }
}

static struct disk {
 // memory for virtio descriptors &c for queue 0.
 // this is a global instead of allocated because it must
 // be multiple contiguous pages, which kalloc()
 // doesn't support, and page aligned.
  char pages[2*PGSIZE];
  struct VRingDesc *desc;
  uint16 *avail;
  struct UsedArea *used;

  // our own book-keeping.
  char free[NUM];  // is a descriptor free?
  uint16 used_idx; // we've looked this far in used[2..NUM].

  /**
   * @brief 跟踪正在进行的请求信息（按描述符链的第一个描述符索引索引）
   */
  struct {
    struct buf *b;               /**< 指向请求对应的 buf */
    struct virtio_blk_req header;/**< virtio 请求头（type/reserved/sector） */
    char status;                 /**< 设备返回的 1 字节状态 */
    uint64 dispatch_us;          /**< 发出请求的时间（微秒），用于计算瞬时带宽 */
  } info[NUM];

  /** @brief mclock 调度相关字段 */
  mclock_queue_state class_queue[k_mclock_class_count]; /**< 每个服务类的等待链表与上次 tag 时间 */
  struct buf *inflight_b;                            /**< 当前正在设备上排队但未完成的 buf */
  int inflight_idx;                                  /**< 对应的描述符索引 */
  uint64 ewma_bps;                                   /**< EWMA 带宽估计（字节/秒） */

  /** @brief 保护 virtio disk 状态的自旋锁 */
  SpinLock vdisk_lock;

} __attribute__ ((aligned (PGSIZE))) disk, disk2;

static void mclock_reset_primary_state();
static void mclock_enqueue_primary_locked(struct buf *b, int write);
static struct buf *mclock_pop_primary_head_locked(int service_class);
static struct buf *mclock_pick_primary_locked(uint64 now_us, uint64 *next_gate_us);
static void mclock_submit_primary_locked(struct buf *b);
static bool mclock_dispatch_primary_locked(uint64 *next_gate_us);

void
virtio_disk_init(void)
{
  uint32 status = 0;

  disk.vdisk_lock.init( "virtio_disk_lock");

  if(*R(VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976 ||
     *R(VIRTIO_MMIO_VERSION) != 1 ||
     *R(VIRTIO_MMIO_DEVICE_ID) != 2 ||
     *R(VIRTIO_MMIO_VENDOR_ID) != 0x554d4551){
    panic("could not find virtio disk");
  }

  status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
  *R(VIRTIO_MMIO_STATUS) = status;

  status |= VIRTIO_CONFIG_S_DRIVER;
  *R(VIRTIO_MMIO_STATUS) = status;

  // negotiate features
  uint64 features = *R(VIRTIO_MMIO_DEVICE_FEATURES);
  features &= ~(1 << VIRTIO_BLK_F_RO);
  features &= ~(1 << VIRTIO_BLK_F_SCSI);
  features &= ~(1 << VIRTIO_BLK_F_CONFIG_WCE);
  features &= ~(1 << VIRTIO_BLK_F_MQ);
  features &= ~(1 << VIRTIO_F_ANY_LAYOUT);
  features &= ~(1 << VIRTIO_RING_F_EVENT_IDX);
  features &= ~(1 << VIRTIO_RING_F_INDIRECT_DESC);
  *R(VIRTIO_MMIO_DRIVER_FEATURES) = features;

  // tell device that feature negotiation is complete.
  status |= VIRTIO_CONFIG_S_FEATURES_OK;
  *R(VIRTIO_MMIO_STATUS) = status;

  // tell device we're completely ready.
  status |= VIRTIO_CONFIG_S_DRIVER_OK;
  *R(VIRTIO_MMIO_STATUS) = status;

  *R(VIRTIO_MMIO_GUEST_PAGE_SIZE) = PGSIZE;

  // initialize queue 0.
  *R(VIRTIO_MMIO_QUEUE_SEL) = 0;
  uint32 max = *R(VIRTIO_MMIO_QUEUE_NUM_MAX);
  if(max == 0)
    panic("virtio disk has no queue 0");
  if(max < NUM)
    panic("virtio disk max queue too short");
  *R(VIRTIO_MMIO_QUEUE_NUM) = NUM;
  memset(disk.pages, 0, sizeof(disk.pages));
  *R(VIRTIO_MMIO_QUEUE_PFN) = ((uint64)disk.pages) >> PGSHIFT;

  // desc = pages -- num * VRingDesc
  // avail = pages + 0x40 -- 2 * uint16, then num * uint16
  // used = pages + 4096 -- 2 * uint16, then num * vRingUsedElem

  disk.desc = (struct VRingDesc *) disk.pages;
  disk.avail = (uint16*)(((char*)disk.desc) + NUM*sizeof(struct VRingDesc));
  disk.used = (struct UsedArea *) (disk.pages + PGSIZE);

  for(int i = 0; i < NUM; i++)
    disk.free[i] = 1;
  mclock_reset_primary_state();

  // plic.c and trap.c arrange for interrupts from VIRTIO0_IRQ.
  #ifdef DEBUG
  printf("virtio_disk_init\n");
  #endif
}

void
virtio_disk_init2(void)
{
  uint32 status = 0;

  disk2.vdisk_lock.init("virtio_disk2");

  if(*R2(VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976 ||
     *R2(VIRTIO_MMIO_VERSION) != 1 ||
     *R2(VIRTIO_MMIO_DEVICE_ID) != 2 ||
     *R2(VIRTIO_MMIO_VENDOR_ID) != 0x554d4551){
    panic("could not find virtio disk2");
  }

  status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
  *R2(VIRTIO_MMIO_STATUS) = status;

  status |= VIRTIO_CONFIG_S_DRIVER;
  *R2(VIRTIO_MMIO_STATUS) = status;

  // negotiate features
  uint64 features = *R2(VIRTIO_MMIO_DEVICE_FEATURES);
  features &= ~(1 << VIRTIO_BLK_F_RO);
  features &= ~(1 << VIRTIO_BLK_F_SCSI);
  features &= ~(1 << VIRTIO_BLK_F_CONFIG_WCE);
  features &= ~(1 << VIRTIO_BLK_F_MQ);
  features &= ~(1 << VIRTIO_F_ANY_LAYOUT);
  features &= ~(1 << VIRTIO_RING_F_EVENT_IDX);
  features &= ~(1 << VIRTIO_RING_F_INDIRECT_DESC);
  *R2(VIRTIO_MMIO_DRIVER_FEATURES) = features;

  // tell device that feature negotiation is complete.
  status |= VIRTIO_CONFIG_S_FEATURES_OK;
  *R2(VIRTIO_MMIO_STATUS) = status;

  // tell device we're completely ready.
  status |= VIRTIO_CONFIG_S_DRIVER_OK;
  *R2(VIRTIO_MMIO_STATUS) = status;

  *R2(VIRTIO_MMIO_GUEST_PAGE_SIZE) = PGSIZE;

  // initialize queue 0.
  *R2(VIRTIO_MMIO_QUEUE_SEL) = 0;
  uint32 max = *R2(VIRTIO_MMIO_QUEUE_NUM_MAX);
  if(max == 0)
    panic("virtio disk2 has no queue 0");
  if(max < NUM)
    panic("virtio disk2 max queue too short");
  *R2(VIRTIO_MMIO_QUEUE_NUM) = NUM;
  memset(disk2.pages, 0, sizeof(disk2.pages));
  *R2(VIRTIO_MMIO_QUEUE_PFN) = ((uint64)disk2.pages) >> PGSHIFT;

  // desc = pages -- num * VRingDesc
  // avail = pages + 0x40 -- 2 * uint16, then num * uint16
  // used = pages + 4096 -- 2 * uint16, then num * vRingUsedElem

  disk2.desc = (struct VRingDesc *) disk2.pages;
  disk2.avail = (uint16*)(((char*)disk2.desc) + NUM*sizeof(struct VRingDesc));
  disk2.used = (struct UsedArea *) (disk2.pages + PGSIZE);

  for(int i = 0; i < NUM; i++)
    disk2.free[i] = 1;

  // plic.c and trap.c arrange for interrupts from VIRTIO0_IRQ.
  #ifdef DEBUG
  printf("virtio_disk_init\n");
  #endif
}

// find a free descriptor, mark it non-free, return its index.
static int
alloc_desc()
{
  for(int i = 0; i < NUM; i++){
    if(disk.free[i]){
      disk.free[i] = 0;
      return i;
    }
  }
  return -1;
}

static int
alloc_desc2()
{
  for(int i = 0; i < NUM; i++){
    if(disk2.free[i]){
      disk2.free[i] = 0;
      return i;
    }
  }
  return -1;
}

// mark a descriptor as free.
static void
free_desc(int i)
{
  if(i >= NUM)
    panic("virtio_disk_intr 1");
  if(disk.free[i])
    panic("virtio_disk_intr 2");
  disk.desc[i].addr = 0;
  disk.desc[i].len = 0;
  disk.desc[i].flags = 0;
  disk.desc[i].next = 0;
  disk.free[i] = 1;
  proc::k_pm.wakeup(&disk.free[0]);
}

static void
free_desc2(int i)
{
  if(i >= NUM)
    panic("virtio_disk2_intr 1");
  if(disk2.free[i])
    panic("virtio_disk2_intr 2");
  disk2.desc[i].addr = 0;
  disk2.desc[i].len = 0;
  disk2.desc[i].flags = 0;
  disk2.desc[i].next = 0;
  disk2.free[i] = 1;
  proc::k_pm.wakeup(&disk2.free[0]);
}

// free a chain of descriptors.
static void
free_chain(int i)
{
  while(1){
    int flag = disk.desc[i].flags;
    int nxt = disk.desc[i].next;
    free_desc(i);
    if(flag & VRING_DESC_F_NEXT)
      i = nxt;
    else
      break;
  }
}

static void
free_chain2(int i)
{
  while(1){
    int flag = disk2.desc[i].flags;
    int nxt = disk2.desc[i].next;
    free_desc2(i);
    if(flag & VRING_DESC_F_NEXT)
      i = nxt;
    else
      break;
  }
}

static int
alloc3_desc(int *idx)
{
  for(int i = 0; i < 3; i++){
    idx[i] = alloc_desc();
    if(idx[i] < 0){
      for(int j = 0; j < i; j++)
        free_desc(idx[j]);
      return -1;
    }
  }
  return 0;
}

static int
alloc3_desc2(int *idx)
{
  for(int i = 0; i < 3; i++){
    idx[i] = alloc_desc2();
    if(idx[i] < 0){
      for(int j = 0; j < i; j++)
        free_desc2(idx[j]);
      return -1;
    }
  }
  return 0;
}

/**
 * @brief 重置 mclock 的运行时状态
 *
 * - 清除当前 inflight 状态，重置 ewma 带宽到默认值
 * - 清空每个服务类的队列与上次 tag 时间，供初始化或恢复使用
 */
static void
mclock_reset_primary_state()
{
  disk.inflight_b = nullptr;
  disk.inflight_idx = -1;
  disk.ewma_bps = k_default_ewma_bps;
  for (int i = 0; i < k_mclock_class_count; ++i)
  {
    disk.class_queue[i].head = nullptr;
    disk.class_queue[i].tail = nullptr;
    disk.class_queue[i].last_r_tag_us = 0;
    disk.class_queue[i].last_w_tag_us = 0;
    disk.class_queue[i].last_l_tag_us = 0;
  }
}

/**
 * @brief 在持锁情况下将 buf 入队到对应服务类并设置调度标签
 *
 * @param b 要入队的 buf
 * @param write 非零表示写操作
 *
 * 处理流程：计算 submit_nice -> service_class，为 buf 计算并设置 io_r_tag_us/io_w_tag_us/io_l_tag_us，
 * 并将 buf 挂到 class_queue 的尾部。最后标记 b->disk = 1 表示已入队等待调度。
 */
static void
mclock_enqueue_primary_locked(struct buf *b, int write)
{
  proc::Pcb *current = proc::k_pm.get_cur_pcb();
  int submit_nice = current ? current->get_priority() : proc::default_proc_prio;
  int service_class = nice_to_service_class(submit_nice);
  uint64 now_us = mclock_now_us();
  uint64 bytes = BSIZE;
  mclock_queue_state &queue = disk.class_queue[service_class];

  b->io_write = write;
  b->io_service_class = service_class;
  b->io_submit_pid = current ? current->get_pid() : 0;
  b->io_submit_nice = submit_nice;
  b->io_enqueue_us = now_us;
  b->io_request_bytes = bytes;
  b->io_next = nullptr;

  uint64 reservation_start = queue.last_r_tag_us > now_us ? queue.last_r_tag_us : now_us;
  b->io_r_tag_us = reservation_start;
  queue.last_r_tag_us = reservation_start + bytes_to_service_us(bytes, k_class_reservation_bps[service_class]);

  uint64 weight_start = queue.last_w_tag_us > now_us ? queue.last_w_tag_us : now_us;
  b->io_w_tag_us = weight_start;
  queue.last_w_tag_us = weight_start + weighted_service_us(bytes, k_class_weight[service_class], disk.ewma_bps);

  if (k_class_limit_bps[service_class] == k_unlimited_bps)
  {
    b->io_l_tag_us = now_us;
    queue.last_l_tag_us = now_us;
  }
  else
  {
    uint64 limit_start = queue.last_l_tag_us > now_us ? queue.last_l_tag_us : now_us;
    b->io_l_tag_us = limit_start;
    queue.last_l_tag_us = limit_start + bytes_to_service_us(bytes, k_class_limit_bps[service_class]);
  }

  if (queue.tail == nullptr)
  {
    queue.head = b;
    queue.tail = b;
  }
  else
  {
    queue.tail->io_next = b;
    queue.tail = b;
  }

  b->disk = 1;
}

/**
 * @brief 从指定服务类队列头弹出一个 buf 并返回
 *
 * @param service_class 服务类索引
 * @return 指向被弹出的 buf，若队列为空返回 nullptr
 * @note 假定调用者已持锁
 */
static struct buf *
mclock_pop_primary_head_locked(int service_class)
{
  mclock_queue_state &queue = disk.class_queue[service_class];
  struct buf *b = queue.head;
  if (b == nullptr)
  {
    return nullptr;
  }

  queue.head = b->io_next;
  if (queue.head == nullptr)
  {
    queue.tail = nullptr;
  }
  b->io_next = nullptr;
  return b;
}

/**
 * @brief 在所有服务类中选择下一个可调度的 buf
 *
 * 选择策略：优先选择满足 r_tag 的请求（保底）；若无保底候选，则在非被 limit 阻挡的请求中选择最小 w_tag 的请求。
 * 若所有候选都被 limit 阻挡，则返回 nullptr，并通过 next_gate_us 返回最早的 l_tag（下一次可服务时间）。
 *
 * @param now_us 当前时间（微秒）
 * @param[out] next_gate_us 若非空，返回最早的 l_tag（微秒）
 * @return 选中的 buf 指针，若无可服务请求返回 nullptr
 */
static struct buf *
mclock_pick_primary_locked(uint64 now_us, uint64 *next_gate_us)
{
  struct buf *best_reservation = nullptr;
  struct buf *best_weight = nullptr;
  uint64 earliest_gate = ~0ULL;

  for (int i = 0; i < k_mclock_class_count; ++i)
  {
    struct buf *candidate = disk.class_queue[i].head;
    if (candidate == nullptr)
    {
      continue;
    }

    if (candidate->io_l_tag_us > now_us)
    {
      if (candidate->io_l_tag_us < earliest_gate)
      {
        earliest_gate = candidate->io_l_tag_us;
      }
      continue;
    }

    if (candidate->io_r_tag_us <= now_us)
    {
      if (best_reservation == nullptr ||
          candidate->io_r_tag_us < best_reservation->io_r_tag_us ||
          (candidate->io_r_tag_us == best_reservation->io_r_tag_us &&
           candidate->io_w_tag_us < best_reservation->io_w_tag_us))
      {
        best_reservation = candidate;
      }
      continue;
    }

    if (best_weight == nullptr ||
        candidate->io_w_tag_us < best_weight->io_w_tag_us ||
        (candidate->io_w_tag_us == best_weight->io_w_tag_us &&
         candidate->io_r_tag_us < best_weight->io_r_tag_us))
    {
      best_weight = candidate;
    }
  }

  if (next_gate_us != nullptr)
  {
    *next_gate_us = earliest_gate;
  }
  return best_reservation ? best_reservation : best_weight;
}

/**
 * @brief 将选中的 buf 构造为 virtio 描述符链并通知设备
 *
 * @param b 要提交的 buf
 *
 * 处理流程：分配 3 个描述符并填充 header/data/status，记录 dispatch_us、inflight_b 和 inflight_idx，
 * 随后写 avail 并触发队列通知。
 */
static void
mclock_submit_primary_locked(struct buf *b)
{
  int idx[3];
  if (alloc3_desc(idx) != 0)
  {
    panic("mclock_submit_primary_locked: no descriptor");
  }

  disk.info[idx[0]].header.type = b->io_write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
  disk.info[idx[0]].header.reserved = 0;
  disk.info[idx[0]].header.sector = b->blockno;
  disk.info[idx[0]].status = 0;
  disk.info[idx[0]].dispatch_us = mclock_now_us();
  disk.info[idx[0]].b = b;

  disk.desc[idx[0]].addr = (uint64)mem::k_pagetable.kwalk_addr((uint64)&disk.info[idx[0]].header);
  disk.desc[idx[0]].len = sizeof(disk.info[idx[0]].header);
  disk.desc[idx[0]].flags = VRING_DESC_F_NEXT;
  disk.desc[idx[0]].next = idx[1];

  disk.desc[idx[1]].addr = (uint64)b->data;
  disk.desc[idx[1]].len = BSIZE;
  disk.desc[idx[1]].flags = b->io_write ? 0 : VRING_DESC_F_WRITE;
  disk.desc[idx[1]].flags |= VRING_DESC_F_NEXT;
  disk.desc[idx[1]].next = idx[2];

  disk.desc[idx[2]].addr = (uint64)&disk.info[idx[0]].status;
  disk.desc[idx[2]].len = 1;
  disk.desc[idx[2]].flags = VRING_DESC_F_WRITE;
  disk.desc[idx[2]].next = 0;

  disk.inflight_b = b;
  disk.inflight_idx = idx[0];

  disk.avail[2 + (disk.avail[1] % NUM)] = idx[0];
  __sync_synchronize();
  disk.avail[1] = disk.avail[1] + 1;
  *R(VIRTIO_MMIO_QUEUE_NOTIFY) = 0;
}

/**
 * @brief 如果没有正在进行的请求，则尝试从队列中调度一个并提交
 *
 * @param[out] next_gate_us 如果未调度且受 limit 阻挡，返回下一次可服务的时间（微秒）
 * @return true 表示已调度并提交一个请求，false 表示未调度
 */
static bool
mclock_dispatch_primary_locked(uint64 *next_gate_us)
{
  if (disk.inflight_b != nullptr)
  {
    if (next_gate_us != nullptr)
    {
      *next_gate_us = 0;
    }
    return false;
  }

  uint64 now_us = mclock_now_us();
  struct buf *selected = mclock_pick_primary_locked(now_us, next_gate_us);
  if (selected == nullptr)
  {
    return false;
  }

  struct buf *queued = mclock_pop_primary_head_locked(selected->io_service_class);
  if (queued != selected)
  {
    panic("mclock_dispatch_primary_locked: queue mismatch");
  }

  mclock_submit_primary_locked(selected);
  if (next_gate_us != nullptr)
  {
    *next_gate_us = 0;
  }
  return true;
}


void
virtio_disk_rw(struct buf *b, int write)
{
  disk.vdisk_lock.acquire();
  mclock_enqueue_primary_locked(b, write);
  while (b->disk == 1) {
    uint64 next_gate_us = 0;
    if (!mclock_dispatch_primary_locked(&next_gate_us)) {
      if (b->disk != 1) {
        break;
      }
      proc::k_pm.sleep(b, &disk.vdisk_lock);
      continue;
    }
    if (b->disk == 1) {
      proc::k_pm.sleep(b, &disk.vdisk_lock);
    }
  }
  disk.vdisk_lock.release();
}

void
virtio_disk_rw2(struct buf *b, int write)
{
  // printf("[virtio_disk_rw] virtio disk rw, cpuid: %d\n", cpuid());
  uint64 sector = b->blockno;

  disk2.vdisk_lock.acquire();

  // the spec says that legacy block operations use three
  // descriptors: one for type/reserved/sector, one for
  // the data, one for a 1-byte status result.

  // allocate the three descriptors.
  int idx[3];
  while(1){
    if(alloc3_desc2(idx) == 0) {
      break;
    }
    proc::k_pm.sleep(&disk2.free[0], &disk2.vdisk_lock);
  }

  // format the three descriptors.
  // qemu's virtio-blk.c reads them.

  struct virtio_blk_outhdr {
    uint32 type;
    uint32 reserved;
    uint64 sector;
  } buf0;

  if(write)
    buf0.type = VIRTIO_BLK_T_OUT; // write the disk
  else
    buf0.type = VIRTIO_BLK_T_IN; // read the disk
  buf0.reserved = 0;
  buf0.sector = sector;

  // buf0 is on a kernel stack, which is not direct mapped,
  // thus the call to kvmpa().
  disk2.desc[idx[0]].addr = (uint64)mem::k_pagetable.kwalkaddr((uint64) &buf0).get_data();
  // disk2.desc[idx[0]].addr = (uint64) &buf0;
  disk2.desc[idx[0]].len = sizeof(buf0);
  disk2.desc[idx[0]].flags = VRING_DESC_F_NEXT;
  disk2.desc[idx[0]].next = idx[1];

  disk2.desc[idx[1]].addr = (uint64)b->data;
  disk2.desc[idx[1]].len = BSIZE;
  if(write)
    disk2.desc[idx[1]].flags = 0; // device reads b->data
  else
    disk2.desc[idx[1]].flags = VRING_DESC_F_WRITE; // device writes b->data
  disk2.desc[idx[1]].flags |= VRING_DESC_F_NEXT;
  disk2.desc[idx[1]].next = idx[2];

  disk2.info[idx[0]].status = 0;
  disk2.desc[idx[2]].addr = (uint64) &disk2.info[idx[0]].status;
  disk2.desc[idx[2]].len = 1;
  disk2.desc[idx[2]].flags = VRING_DESC_F_WRITE; // device writes the status
  disk2.desc[idx[2]].next = 0;

  // record struct buf for virtio_disk2_intr().
  b->disk = 2;
  disk2.info[idx[0]].b = b;

  // avail[0] is flags
  // avail[1] tells the device how far to look in avail[2...].
  // avail[2...] are desc[] indices the device should process.
  // we only tell device the first index in our chain of descriptors.
  disk2.avail[2 + (disk2.avail[1] % NUM)] = idx[0];
  __sync_synchronize();
  disk2.avail[1] = disk2.avail[1] + 1;

  *R2(VIRTIO_MMIO_QUEUE_NOTIFY) = 0; // value is queue number

  // Wait for virtio_disk_intr() to say request has finished.
  while(b->disk == 2) {
    proc::k_pm.sleep(b, &disk2.vdisk_lock);
  }

  disk2.info[idx[0]].b = 0;
  free_chain2(idx[0]);

  disk2.vdisk_lock.release();
  // printf("[virtio_disk_rw] done, cpuid: %d\n", cpuid());
}

void
virtio_disk_intr()
{
  disk.vdisk_lock.acquire();

  while((disk.used_idx % NUM) != (disk.used->id % NUM)){
    int id = disk.used->elems[disk.used_idx].id;

    if(disk.info[id].status != 0)
      panic("virtio_disk_intr status");

    struct buf *done = disk.info[id].b;
    uint64 finish_us = mclock_now_us();
    /**
     * @brief 更新 EWMA 带宽估计
     *
     * 当请求完成且有有效 dispatch 时间时，计算该请求的瞬时带宽 instant_bps = bytes / elapsed_seconds，
     * 并用 7/8 老值 + 1/8 新值的方式更新 disk.ewma_bps。该估计用于后续的权重调度时间窗计算（weighted_service_us）。
     */
    if (done != nullptr && disk.info[id].dispatch_us != 0 && finish_us > disk.info[id].dispatch_us)
    {
      uint64 elapsed_us = finish_us - disk.info[id].dispatch_us;
      uint64 instant_bps = ceil_div_u64(done->io_request_bytes * k_usec_per_sec, elapsed_us);
      if (instant_bps != 0)
      {
        disk.ewma_bps = (disk.ewma_bps * 7 + instant_bps) / 8;
      }
    }

    if (done != nullptr)
    {
      done->disk = 0;
    }
    disk.info[id].b = 0;
    disk.info[id].dispatch_us = 0;
    free_chain(id);
    disk.inflight_b = nullptr;
    disk.inflight_idx = -1;

    disk.used_idx = (disk.used_idx + 1) % NUM;
    mclock_dispatch_primary_locked(nullptr);
    if (done != nullptr)
    {
      proc::k_pm.wakeup(done);
    }
  }
  *R(VIRTIO_MMIO_INTERRUPT_ACK) = *R(VIRTIO_MMIO_INTERRUPT_STATUS) & 0x3;

  disk.vdisk_lock.release();
}

void
virtio_disk_intr2()
{
  // printf("[virtio_disk_intr] virtio_disk_intr!\n");
  disk2.vdisk_lock.acquire();

  while((disk2.used_idx % NUM) != (disk2.used->id % NUM)){
    int id = disk2.used->elems[disk2.used_idx].id;

    if(disk2.info[id].status != 0)
      panic("virtio_disk_intr status");

    disk2.info[id].b->disk = 0;   // disk is done with buf
    proc::k_pm.wakeup(disk2.info[id].b);

    disk2.used_idx = (disk2.used_idx + 1) % NUM;
  }
  *R2(VIRTIO_MMIO_INTERRUPT_ACK) = *R2(VIRTIO_MMIO_INTERRUPT_STATUS) & 0x3;

  disk2.vdisk_lock.release();
  // printf("[virtio_disk_intr] done!\n");
}
#endif
