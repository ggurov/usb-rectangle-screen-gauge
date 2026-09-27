/*
 * app_console.h - the REPL, on stdin.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void app_console_start(void);

/* Reads whatever has been typed; runs a command on Enter.  Never blocks. */
void app_console_poll(void);

/* True after `quit` (or EOF). */
bool app_console_wants_quit(void);

#ifdef __cplusplus
}
#endif
