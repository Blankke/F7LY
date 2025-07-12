#pragma once

#ifndef __TIME_H__
#define __TIME_H__

#include "types.hh"


#define FREQUENCY 10000000L // qemu时钟频率12500000

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

typedef struct timespecc {
    uint64 tv_sec; /* Seconds */
    uint64 tv_nsec; /* Nanoseconds */
} timespec_t;

struct timevall {
    uint64 tv_sec; /* Seconds */
    uint64 tv_usec; /* Microseconds */
};
extern uint ticks;

struct tms {
    long tms_utime;
    long tms_stime;
    long tms_cutime;
    long tms_cstime;
};

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
