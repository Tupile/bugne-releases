// logstore: mirror esp_log output into a PSRAM ring buffer (see logstore.h).
#include "logstore.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static char *s_buf;                  // PSRAM ring buffer, LOGSTORE_SIZE bytes
static size_t s_head;                // next write index
static bool s_wrapped;               // buffer filled once and wrapped
static SemaphoreHandle_t s_lock;
static vprintf_like_t s_prev;        // previous (console) output function
static char s_line[512];             // formatting scratch, guarded by s_lock

// Append len bytes to the ring buffer (caller holds s_lock).
static void buf_append(const char *data, int len)
{
    for (int i = 0; i < len; i++) {
        s_buf[s_head++] = data[i];
        if (s_head >= LOGSTORE_SIZE) {
            s_head = 0;
            s_wrapped = true;
        }
    }
}

static int log_vprintf(const char *fmt, va_list args)
{
    // Always print to the console first (consume a private copy of args).
    va_list copy;
    va_copy(copy, args);
    int n = s_prev ? s_prev(fmt, copy) : 0;
    va_end(copy);

    // Then capture the same line. Skip if the buffer is busy (never block the
    // console path on a reader).
    if (s_buf && s_lock && xSemaphoreTake(s_lock, 0) == pdTRUE) {
        int len = vsnprintf(s_line, sizeof(s_line), fmt, args);
        if (len > 0) {
            if (len > (int)sizeof(s_line) - 1) {
                len = sizeof(s_line) - 1;  // line was truncated
            }
            buf_append(s_line, len);
        }
        xSemaphoreGive(s_lock);
    }
    return n;
}

esp_err_t logstore_init(void)
{
    s_buf = heap_caps_malloc(LOGSTORE_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_buf) {
        return ESP_ERR_NO_MEM;
    }
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        heap_caps_free(s_buf);
        s_buf = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_prev = esp_log_set_vprintf(log_vprintf);
    return ESP_OK;
}

void logstore_read(char *buf, size_t size, size_t *out_len)
{
    size_t n = 0;
    if (size == 0) {
        if (out_len) *out_len = 0;
        return;
    }
    size_t cap = size - 1;  // leave room for the null terminator
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    if (s_buf) {
        if (s_wrapped) {
            // Oldest byte is at s_head; copy s_head..end, then 0..s_head.
            size_t tail = LOGSTORE_SIZE - s_head;
            size_t c1 = tail < cap ? tail : cap;
            memcpy(buf, s_buf + s_head, c1);
            n = c1;
            size_t c2 = s_head < (cap - n) ? s_head : (cap - n);
            memcpy(buf + n, s_buf, c2);
            n += c2;
        } else {
            size_t c = s_head < cap ? s_head : cap;
            memcpy(buf, s_buf, c);
            n = c;
        }
    }
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
    buf[n] = '\0';
    if (out_len) *out_len = n;
}
