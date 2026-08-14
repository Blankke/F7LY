//
// Copied from Li Shuang ( pseudonym ) on 2024-07-29
// --------------------------------------------------------------
// | Note: This code file just for study, not for commercial use
// | Contact Author: lishuang.mk@whu.edu.cn
// --------------------------------------------------------------
//

#pragma once
namespace tmm
{
	struct timespec;
}
#include "types.hh"
namespace proc
{
	class Pcb;
	class ProcessMemoryManager;

	// FUTEX_PRIVATE_FLAG 不能只靠“当前物理页”建键：fork 后私有页会 COW，
	// 物理页会变化，但同一地址空间内等待者和唤醒者仍必须相互匹配。共享
	// futex 则继续以共享物理页为身份。完整 key 只在桶内二次比较；hash
	// 只用于选桶，不能承担身份判等职责。
	enum class FutexKeyScope : uint8
	{
		Auto,
		Private,
	};

	struct FutexKey
	{
		uint64 value = 0;
		ProcessMemoryManager *private_mm = nullptr;
		bool is_private = false;

		bool valid() const { return value != 0; }
	};

	inline bool futex_keys_equal(const FutexKey &lhs, const FutexKey &rhs)
	{
		if (lhs.value != rhs.value || lhs.is_private != rhs.is_private)
		{
			return false;
		}
		return !lhs.is_private || lhs.private_mm == rhs.private_mm;
	}

	// following code is from linux (include/uapi/linux/futex.h)

	/*
	 * Support for robust futexes: the kernel cleans up held futexes at
	 * thread exit time.
	 */

	/*
	 * Per-lock list entry - embedded in user-space locks, somewhere close
	 * to the futex field. (Note: user-space uses a double-linked list to
	 * achieve O(1) list add and remove, but the kernel only needs to know
	 * about the forward link)
	 *
	 * NOTE: this structure is part of the syscall ABI, and must not be
	 * changed.
	 */
	struct robust_list
	{
		struct robust_list *next;
	};

	/*
	 * Per-thread list head:
	 *
	 * NOTE: this structure is part of the syscall ABI, and must only be
	 * changed if the change is first communicated with the glibc folks.
	 * (When an incompatible change is done, we'll increase the structure
	 *  size, which glibc will detect)
	 */
	struct robust_list_head
	{
		/*
		 * The head of the list. Points back to itself if empty:
		 */
		robust_list list;

		/*
		 * This relative offset is set by user-space, it gives the kernel
		 * the relative position of the futex field to examine. This way
		 * we keep userspace flexible, to freely shape its data-structure,
		 * without hardcoding any particular offset into the kernel:
		 */
		long futex_offset;

		/*
		 * The death of the thread may race with userspace setting
		 * up a lock's links. So to handle this race, userspace first
		 * sets this field to the address of the to-be-taken lock,
		 * then does the lock acquire, and then adds itself to the
		 * list, and then clears this field. Hence the kernel will
		 * always have full knowledge of all locks that the thread
		 * _might_ have taken. We check the owner TID in any case,
		 * so only truly owned locks will be handled.
		 */
		robust_list *list_op_pending;
	};

	int futex_wait(uint64 uaddr, int val, tmm::timespec *timeout,
	               bool timeout_is_absolute = false,
	               bool use_realtime_clock = false,
	               FutexKeyScope key_scope = FutexKeyScope::Auto);
	int futex_wakeup(uint64 uaddr, int val, void *uaddr2, int val2,
	                 FutexKeyScope key_scope = FutexKeyScope::Auto,
	                 bool include_requeued_in_result = false,
	                 const int *expected_requeue_value = nullptr);
	int futex_wake_op(uint64 uaddr1, int wake_count, uint64 uaddr2,
	                  int second_wake_count, uint32 encoded_op,
	                  FutexKeyScope key_scope = FutexKeyScope::Auto);
	// 清理 PCB 在 futex 桶中的等待者登记。调用方可以持有 PCB 锁；函数返回时
	// 保持调用前的 PCB 锁状态，供信号、退出和 PCB 回收路径统一摘链。
	void futex_remove_waiter(Pcb *p);
	// 由唯一的 timekeeper CPU 在每次 tick 中检查定时 futex；超时等待不再
	// 挂到全局 ticks channel，避免每个 tick 唤醒所有无期限 futex 等待者。
	void futex_check_timeouts(uint64 now_cycles);
	void futex_cleanup_robust_list(struct robust_list_head *head);

} // namespace pm
// futex
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_FD 2
#define FUTEX_REQUEUE 3
#define FUTEX_CMP_REQUEUE 4
#define FUTEX_WAKE_OP 5
#define FUTEX_LOCK_PI 6
#define FUTEX_UNLOCK_PI 7
#define FUTEX_TRYLOCK_PI 8
#define FUTEX_WAIT_BITSET 9
#define FUTEX_WAKE_BITSET 10
#define FUTEX_WAIT_REQUEUE_PI 11
#define FUTEX_CMP_REQUEUE_PI 12
#define FUTEX_LOCK_PI2 13

#define FUTEX_PRIVATE_FLAG 128
#define FUTEX_COUNT 2048

#define FUTEX_CLOCK_REALTIME 256

#define FUTEX_CMD_MASK ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME)

// Robust futex constants
#define FUTEX_OWNER_DIED	0x40000000  // Mutex owner died
#define FUTEX_WAITERS		0x80000000  // Has waiters bit
#define FUTEX_TID_MASK		0x3fffffff  // Thread ID mask

#define EAGAIN 11
