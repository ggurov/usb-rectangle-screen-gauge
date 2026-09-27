/*
 * app_time.c - clock and sleep for the frame loop.
 *
 * The frame loop measures its own interval, so the clock must not step under
 * it mid-frame: QueryPerformanceCounter on Windows, CLOCK_MONOTONIC
 * elsewhere.
 */
#include "app_time.h"

#include <stdbool.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>

static LARGE_INTEGER s_freq;
static bool s_started;

void app_time_init(void)
{
    if (!s_started) {
        QueryPerformanceFrequency(&s_freq);
        timeBeginPeriod(1);   /* otherwise Sleep(1) is a 15 ms roulette */
        s_started = true;
    }
}

uint64_t app_now_us(void)
{
    if (!s_started) {
        app_time_init();
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (uint64_t)((double)now.QuadPart * 1e6 / (double)s_freq.QuadPart);
}

void app_sleep_ms(int ms)
{
    if (ms > 0) {
        Sleep((DWORD)ms);
    }
}

#else

#include <time.h>

void app_time_init(void) {}

uint64_t app_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

void app_sleep_ms(int ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

#endif
