/**
 * @file time.h
 * @brief 时间管理的遗留C风格接口定义（已废弃）
 * 
 * 警告：本文件为早期实现的C风格时间接口，现已被time.hh替代。
 * 建议新代码使用time.hh中的C++接口，该文件仅为兼容性保留。
 * 
 */

#pragma once

#ifndef __TIME_H__
#define __TIME_H__

#include "types.hh"

// 注意：以下时钟频率定义可能已过时，请参考time.hh中的qemu_fre
#define FREQUENCY 10000000L // qemu时钟频率12500000

// 时间转换宏（建议使用time.hh中的内联函数替代）
#define TIME2SEC(time) (time / FREQUENCY)
#define TIME2MS(time) (time * 1000 / FREQUENCY)
#define TIME2US(time) (time * 1000 * 1000 / FREQUENCY)
#define TIME2NS(time) (time * 1000 * 1000 * 1000 / FREQUENCY)

#define TIMESEPC2NS(sepc) (sepc.tv_nsec + sepc.tv_sec * 1000 * 1000 * 1000)
#define TIMEVAL2NS(val) (val.tv_usec * 1000 + val.tv_sec * 1000000000)
#define TIMESEPC2SEC(sepc) (sepc.tv_sec + sepc.tv_nsec / (1000 * 1000 * 1000))
#define TIME2TIMESPEC(time)                                                                                            \
(struct timespecc) { .tv_sec = TIME2SEC(time), .tv_nsec = TIME2NS(time) % (1000 * 1000 * 1000) }

#define TIME2TIMEVAL(time)                                                                                             \
(struct timevall) { .tv_sec = TIME2SEC(time), .tv_usec = TIME2US(time) % (1000 * 1000) }

// 遗留的时间结构体定义（建议使用time.hh中的tmm::timespec）
typedef struct timespecc {
    uint64 tv_sec; /* Seconds */
    uint64 tv_nsec; /* Nanoseconds */
} timespec_t;

struct timevall {
    uint64 tv_sec; /* Seconds */
    uint64 tv_usec; /* Microseconds */
};

extern uint ticks; // 全局tick计数（已移至TimerManager管理）

// 进程时间统计结构体（建议使用time.hh中的tmm::tms）
struct tms {
    long tms_utime;
    long tms_stime;
    long tms_cutime;
    long tms_cstime;
};

// 时钟ID宏定义（建议使用time.hh中的tmm::SystemClockId枚举）
#define CLOCK_REALTIME			0
#define CLOCK_MONOTONIC			1
#define CLOCK_PROCESS_CPUTIME_ID	2
#define CLOCK_THREAD_CPUTIME_ID		3
#define CLOCK_MONOTONIC_RAW		4
#define CLOCK_REALTIME_COARSE		5
#define CLOCK_MONOTONIC_COARSE		6
#define CLOCK_BOOTTIME			7
#define CLOCK_REALTIME_ALARM		8
#define CLOCK_BOOTTIME_ALARM		9
#define CLOCK_TAI                      11

#endif
