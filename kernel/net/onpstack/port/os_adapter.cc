/*
 * 版权属于onps栈开发团队，遵循Apache License 2.0开源许可协议
 *
 */
#include "port/datatype.hh"
#include "port/sys_config.hh"
#include "onps_errors.hh"
#include "port/os_datatype.hh"
#include "one_shot_timer.hh"
#include "onps_utils.hh"
#include "protocols.hh"
#include "onps_input.hh"
#include "printer.hh"
#include "timer_manager.hh"
#include "proc/sleeplock.hh"
#include "semaphore.hh"
#if SUPPORT_PPP
#include "ppp/negotiation_storage.hh"
#include "ppp/ppp.hh"
#endif

#include "ip/tcp.hh"

#define SYMBOL_GLOBALS
#include "port/os_adapter.hh"
#undef SYMBOL_GLOBALS

#if SUPPORT_PPP
//* 在此指定连接modem的串行口，以此作为tty终端进行ppp通讯，其存储索引应与os_open_tty()返回的tty句柄值一一对应
const CHAR *or_pszaTTY[PPP_NETLINK_NUM] = {"SCP3"};
const ST_DIAL_AUTH_INFO or_staDialAuth[PPP_NETLINK_NUM] = {
	{"4gnet", "card", "any_char"}, /* 注意ppp账户和密码尽量控制在20个字节以内，太长需要需要修改chap.c中send_response()函数的szData数组容量及 */
								   /* pap.c中pap_send_auth_request()函数的ubaPacket数组的容量，确保其能够封装一个完整的响应报文              */
};
ST_PPPNEGORESULT o_staNegoResult[PPP_NETLINK_NUM] = {
	{{0, PPP_MRU, ACCM_INIT, {PPP_CHAP, 0x05 /* 对于CHAP协议来说，0-4未使用，0x05代表采用MD5算法 */}, TRUE, TRUE, FALSE, FALSE},
	 {IP_ADDR_INIT, DNS_ADDR_INIT, DNS_ADDR_INIT, IP_ADDR_INIT, MASK_INIT},
	 0},

	/* 系统存在几路ppp链路，就在这里添加几路的协商初始值，如果不确定，可以直接将上面预定义的初始值直接复制过来即可 */
};
#endif

#if SUPPORT_ETHERNET
const CHAR *or_pszaEthName[ETHERNET_NUM] = {
	"eth0"};
#endif

//* 协议栈内部工作线程列表
const static STCB_PSTACKTHREAD lr_stcbaPStackThread[] = {
	{thread_one_shot_timer_count, NULL},
#if SUPPORT_PPP
//* 在此按照顺序建立ppp工作线程，入口函数为thread_ppp_handler()，线程入口参数为os_open_tty()返回的tty句柄值
//* 其直接强行进行数据类型转换即可，即作为线程入口参数时直接以如下形式传递：
//* (void *)nPPPIdx
//* 不要传递参数地址，即(void *)&nPPPIdx，这种方式是错误的
#endif

#if SUPPORT_SACK
	{thread_tcp_handler, NULL},
#endif
};

/* 用户自定义变量声明区 */
/* …… */

//* 当前线程休眠指定的秒数，参数unSecs指定要休眠的秒数
void os_sleep_secs(UINT unSecs)
{
	tmm::timeval sleep_tv;
	sleep_tv.tv_sec = unSecs;
	sleep_tv.tv_usec = 0;
	tmm::k_tm.sleep_from_tv(sleep_tv);
}

//* 当前线程休眠指定的毫秒数，单位：毫秒
void os_sleep_ms(UINT unMSecs)
{
	tmm::timeval sleep_tv;
	sleep_tv.tv_sec = unMSecs / 1000;
	sleep_tv.tv_usec = (unMSecs % 1000) * 1000; // 转换为微秒
	tmm::k_tm.sleep_from_tv(sleep_tv);
}

//* 获取系统启动以来已运行的秒数（从0开始）
UINT os_get_system_secs(void)
{
	// panic("os_get_system_secs() cannot be empty");
	uint secs = tmm::k_tm.clock_gettime_sec(tmm::SystemClockId::CLOCK_MONOTONIC);

	return secs;
}

//* 获取系统启动以来已运行的毫秒数（从0开始）
UINT os_get_system_msecs(void)
{
	// panic("os_get_system_msecs() cannot be empty");
	uint msecs = tmm::k_tm.clock_gettime_msec(tmm::SystemClockId::CLOCK_MONOTONIC);

	return msecs;
}

void os_thread_onpstack_start(void *pvParam)
{
	panic("os_thread_onpstack_start() cannot be empty");

	//* 建立工作线程
	INT i;
	for (i = 0; i < (INT)(sizeof(lr_stcbaPStackThread) / sizeof(STCB_PSTACKTHREAD)); i++)
	{
		//* 在此按照顺序建立工作线程
	}

	/* 用户自定义代码 */
	/* …… */
}

HMUTEX os_thread_mutex_init(void)
{
	// 分配内存来存储 SleepLock 对象
	proc::SleepLock* mutex = new proc::SleepLock();
	if (mutex == nullptr) {
		return INVALID_HMUTEX; // 内存分配失败
	}
	
	// 初始化睡眠锁
	mutex->init("onpstack_mutex_lock", "onpstack_mutex");
	
	// 将指针转换为句柄返回
	return reinterpret_cast<HMUTEX>(mutex);
}

void os_thread_mutex_lock(HMUTEX hMutex)
{
	if (hMutex == INVALID_HMUTEX) {
		panic("os_thread_mutex_lock: invalid mutex handle");
		return;
	}
	
	// 将句柄转换为 SleepLock 指针
	proc::SleepLock* mutex = reinterpret_cast<proc::SleepLock*>(hMutex);
	
	// 获取锁
	mutex->acquire();
}

void os_thread_mutex_unlock(HMUTEX hMutex)
{
	if (hMutex == INVALID_HMUTEX) {
		panic("os_thread_mutex_unlock: invalid mutex handle");
		return;
	}
	
	// 将句柄转换为 SleepLock 指针
	proc::SleepLock* mutex = reinterpret_cast<proc::SleepLock*>(hMutex);
	
	// 释放锁
	mutex->release();
}

void os_thread_mutex_uninit(HMUTEX hMutex)
{
	if (hMutex == INVALID_HMUTEX) {
		panic("os_thread_mutex_uninit: invalid mutex handle");
		return;
	}
	
	// 将句柄转换为 SleepLock 指针
	proc::SleepLock* mutex = reinterpret_cast<proc::SleepLock*>(hMutex);
	
	// 释放 SleepLock 对象的内存
	delete mutex;
}

HSEM os_thread_sem_init(UINT unInitVal, UINT unCount)
{
	// 分配内存来存储信号量对象
	sem* semaphore = new sem();
	if (semaphore == nullptr) {
		return INVALID_HSEM; // 内存分配失败
	}
	
	// 使用新的带最大值限制的初始化函数
	if (unCount > 0) {
		// 有最大值限制
		sem_init_with_max(semaphore, (int)unInitVal, (int)unCount, "onpstack_semaphore");
	} else {
		// 无最大值限制
		sem_init(semaphore, (int)unInitVal, "onpstack_semaphore");
	}
	
	// 将指针转换为句柄返回
	return reinterpret_cast<HSEM>(semaphore);
}

void os_thread_sem_post(HSEM hSem)
{
	if (hSem == INVALID_HSEM) {
		panic("os_thread_sem_post: invalid semaphore handle");
		return;
	}
	
	// 将句柄转换为信号量指针
	sem* semaphore = reinterpret_cast<sem*>(hSem);
	
	// 执行V操作，检查是否成功
	if (!sem_try_v(semaphore)) {
		// 达到最大值限制，使用阻塞版本（可能在某些情况下仍然有效）
		sem_v(semaphore);
	}
}

INT os_thread_sem_pend(HSEM hSem, INT nWaitSecs)
{
	if (hSem == INVALID_HSEM) {
		panic("os_thread_sem_pend: invalid semaphore handle");
		return -1;
	}
	
	// 将句柄转换为信号量指针
	sem* semaphore = reinterpret_cast<sem*>(hSem);
	
	if (nWaitSecs == 0) {
		// 永久等待直到信号量可用
		sem_p(semaphore);
		return 0; // 成功获取信号量
	} else {
		// 支持超时的等待，使用新的非阻塞接口
		
		// 记录开始时间
		uint64 start_time = tmm::k_tm.clock_gettime_msec(tmm::CLOCK_MONOTONIC);
		if (start_time == (uint64)-1) {
			return -1; // 获取时间失败
		}
		
		uint64 timeout_ms = (uint64)nWaitSecs * 1000; // 转换为毫秒
		uint64 end_time = start_time + timeout_ms;
		
		// 轮询检查信号量状态，使用适当的睡眠间隔
		while (true) {
			// 先尝试非阻塞获取信号量
			if (sem_try_p(semaphore)) {
				return 0; // 成功获取信号量
			}
			
			// 检查是否超时
			uint64 current_time = tmm::k_tm.clock_gettime_msec(tmm::CLOCK_MONOTONIC);
			if (current_time == (uint64)-1) {
				return -1; // 获取时间失败
			}
			
			if (current_time >= end_time) {
				return 1; // 超时
			}
			
			// 计算剩余时间，选择合适的睡眠间隔
			uint64 remaining_ms = end_time - current_time;
			uint64 sleep_ms = (remaining_ms > 50) ? 10 : 1; // 剩余时间长时睡10ms，短时睡1ms
			
			// 使用项目的睡眠机制进行短暂休眠，避免忙等待
			tmm::timeval sleep_tv;
			sleep_tv.tv_sec = 0;
			sleep_tv.tv_usec = sleep_ms * 1000; // 转换为微秒
			tmm::k_tm.sleep_from_tv(sleep_tv);
		}
	}
}

void os_thread_sem_uninit(HSEM hSem)
{
	if (hSem == INVALID_HSEM) {
		panic("os_thread_sem_uninit: invalid semaphore handle");
		return;
	}
	
	// 将句柄转换为信号量指针
	sem* semaphore = reinterpret_cast<sem*>(hSem);
	
	// 释放信号量对象的内存
	delete semaphore;
}

#if SUPPORT_PPP
HTTY os_open_tty(const CHAR *pszTTYName)
{
#error os_open_tty() cannot be empty

	return INVALID_HTTY;
}

void os_close_tty(HTTY hTTY)
{
#error os_close_tty() cannot be empty
}

INT os_tty_send(HTTY hTTY, UCHAR *pubData, INT nDataLen)
{
#error os_tty_send() cannot be empty

	return 0;
}

INT os_tty_recv(HTTY hTTY, UCHAR *pubRcvBuf, INT nRcvBufLen, INT nWaitSecs)
{
#error os_tty_recv() cannot be empty

	return 0;
}

void os_modem_reset(HTTY hTTY)
{
	/* 用户自定义代码，不需要复位modem设备则这里可以不进行任何操作 */
	/* …… */
}
#endif
