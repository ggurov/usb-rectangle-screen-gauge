/*
 * app_tests.h - the bring-up screens, on the host panel.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* `name` is one of fill, bars, grid, circle, quad; NULL redraws the current. */
void app_tests_show(const char *name);
void app_tests_next(void);
const char *app_tests_current(void);

#ifdef __cplusplus
}
#endif
