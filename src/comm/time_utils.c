/*
 * utils.c
 *
 *  Created on: 2012-7-18
 *      Author: yerungui
 */

#include "time_utils.h"

#ifdef _WIN32
#include <windows.h>

uint64_t gettickcount() {
    return GetTickCount();
}

uint64_t clock_app_monotonic() {
    return gettickcount();
}
#else
static uint64_t clock_milliseconds(clockid_t clock) {
    struct timespec ts;
    if (clock_gettime(clock, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

uint64_t gettickcount() {
#ifdef __ANDROID__
    return clock_milliseconds(CLOCK_BOOTTIME);
#else
    return clock_milliseconds(CLOCK_MONOTONIC);
#endif
}

uint64_t clock_app_monotonic() {
#ifdef __ANDROID__
    return clock_milliseconds(CLOCK_MONOTONIC_RAW);
#else
    return gettickcount();
#endif
}
#endif

int64_t gettickspan(uint64_t _old_tick) {
    uint64_t cur_tick = gettickcount();
    if (_old_tick > cur_tick)
        return 0;

    return cur_tick - _old_tick;
}

uint64_t timeMs() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}
