#ifndef TSFP_HITCH_TRACE_H
#define TSFP_HITCH_TRACE_H

#include <android/log.h>
#include <stdint.h>
#include <time.h>

static inline int64_t tsfp_hitch_now_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}

#define TSFP_HITCH_SPAN(start, end, threshold_ms, format, ...) do {             \
    const int64_t hitch_ns = (end) - (start);                                   \
    if (hitch_ns >= (int64_t)(threshold_ms) * 1000000)                          \
        __android_log_print(ANDROID_LOG_INFO, "TSFP-Hitch",                    \
                            "%.2f ms " format, hitch_ns / 1e6, __VA_ARGS__);   \
} while (0)

#define TSFP_HITCH_BEGIN(name) const int64_t name = tsfp_hitch_now_ns()
#define TSFP_HITCH_END(name, threshold_ms, ...) \
    TSFP_HITCH_SPAN(name, tsfp_hitch_now_ns(), threshold_ms, __VA_ARGS__)

#endif
