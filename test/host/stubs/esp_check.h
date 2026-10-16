// Host-test stub: ESP_RETURN_ON_ERROR without the log line. Not the real header.
#pragma once
#include "esp_err.h"
#define ESP_RETURN_ON_ERROR(x, tag, msg, ...) do { \
    esp_err_t err_rc_ = (x); \
    if (err_rc_ != ESP_OK) { \
        return err_rc_; \
    } \
} while(0)
