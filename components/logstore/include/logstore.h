// logstore: capture recent log output into a PSRAM ring buffer so it can be
// retrieved over Wi-Fi (the config page serves it at /api/logs). All esp_log
// output is still printed to the console as usual.
#pragma once

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Size of the captured log window in bytes (oldest lines roll off).
#define LOGSTORE_SIZE (16 * 1024)

// Install the log hook. Call once, as early as possible in app_main, so boot
// logs are captured too. Allocates the ring buffer in PSRAM.
esp_err_t logstore_init(void);

// Copy the captured log text (oldest first, null-terminated) into buf, at most
// size bytes including the null. Sets *out_len (excluding the null) if not NULL.
void logstore_read(char *buf, size_t size, size_t *out_len);

#ifdef __cplusplus
}
#endif
