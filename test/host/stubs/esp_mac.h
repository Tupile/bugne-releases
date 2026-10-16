// Host-test stub: a fixed station MAC (12:34:56:78:9A:BC), or a scripted
// failure via g_mock_mac_fail. Not the real header.
#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#define ESP_MAC_WIFI_STA 0

extern bool g_mock_mac_fail;

static inline esp_err_t esp_read_mac(uint8_t *mac, int type) {
    if (g_mock_mac_fail) return ESP_FAIL;
    if (type == ESP_MAC_WIFI_STA) {
        mac[0] = 0x12;
        mac[1] = 0x34;
        mac[2] = 0x56;
        mac[3] = 0x78;
        mac[4] = 0x9A;
        mac[5] = 0xBC;
        return ESP_OK;
    }
    return ESP_FAIL;
}
