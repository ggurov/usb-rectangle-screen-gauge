/*
 * app_time.h - a monotonic clock and a sleep, in microseconds.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Asks the OS for the finest timer it has (Windows: 1 ms). Idempotent. */
void app_time_init(void);

/* Microseconds since an arbitrary epoch; never goes backwards. */
uint64_t app_now_us(void);

void app_sleep_ms(int ms);

#ifdef __cplusplus
}
#endif
