// Host-test stub: log macros become no-ops that still reference the tag,
// so `static const char *TAG` stays "used" under -Wall. Not the real header.
// esp_log_set_vprintf records the hook in esp_log_stub_vprintf so a test can
// feed lines through it (logstore installs its capture function this way).
#pragma once
#include <stdarg.h>

typedef int (*vprintf_like_t)(const char *fmt, va_list args);

static vprintf_like_t esp_log_stub_vprintf __attribute__((unused));

static inline vprintf_like_t esp_log_set_vprintf(vprintf_like_t fn)
{
    vprintf_like_t prev = esp_log_stub_vprintf;
    esp_log_stub_vprintf = fn;
    return prev;
}

#define ESP_LOGE(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGD(tag, ...) ((void)(tag))
#define ESP_LOGV(tag, ...) ((void)(tag))
