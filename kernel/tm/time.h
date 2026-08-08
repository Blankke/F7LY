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

#define TIMESEPC2NS(sepc) (sepc.tv_nsec + sepc.tv_sec * 1000 * 1000 * 1000)
#define TIMEVAL2NS(val) (val.tv_usec * 1000 + val.tv_sec * 1000000000)
#define TIMESEPC2SEC(sepc) (sepc.tv_sec + sepc.tv_nsec / (1000 * 1000 * 1000))
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

// C++ 代码使用常量而不是宏，避免破坏 tmm::CLOCK_REALTIME 这类带命名空间的名称。
// 仅保留 C 分支的宏形式，供遗留 C 编译单元使用。
#ifdef __cplusplus
#ifndef CLOCK_REALTIME
inline constexpr int CLOCK_REALTIME = 0;
#endif
#ifndef CLOCK_MONOTONIC
inline constexpr int CLOCK_MONOTONIC = 1;
#endif
#ifndef CLOCK_PROCESS_CPUTIME_ID
inline constexpr int CLOCK_PROCESS_CPUTIME_ID = 2;
#endif
#ifndef CLOCK_THREAD_CPUTIME_ID
inline constexpr int CLOCK_THREAD_CPUTIME_ID = 3;
#endif
#ifndef CLOCK_MONOTONIC_RAW
inline constexpr int CLOCK_MONOTONIC_RAW = 4;
#endif
#ifndef CLOCK_REALTIME_COARSE
inline constexpr int CLOCK_REALTIME_COARSE = 5;
#endif
#ifndef CLOCK_MONOTONIC_COARSE
inline constexpr int CLOCK_MONOTONIC_COARSE = 6;
#endif
#ifndef CLOCK_BOOTTIME
inline constexpr int CLOCK_BOOTTIME = 7;
#endif
#ifndef CLOCK_REALTIME_ALARM
inline constexpr int CLOCK_REALTIME_ALARM = 8;
#endif
#ifndef CLOCK_BOOTTIME_ALARM
inline constexpr int CLOCK_BOOTTIME_ALARM = 9;
#endif
#ifndef CLOCK_TAI
inline constexpr int CLOCK_TAI = 11;
#endif
#else
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID 3
#define CLOCK_MONOTONIC_RAW 4
#define CLOCK_REALTIME_COARSE 5
#define CLOCK_MONOTONIC_COARSE 6
#define CLOCK_BOOTTIME 7
#define CLOCK_REALTIME_ALARM 8
#define CLOCK_BOOTTIME_ALARM 9
#define CLOCK_TAI 11
#endif

#endif
