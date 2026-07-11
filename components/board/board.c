// board: the single shared I2C bus and the device identity.
#include "board.h"
#include "board_pins.h"

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"

static const char *TAG = "board";

static i2c_master_bus_handle_t s_i2c_bus;
static char s_device_id[5] = "0000";     // 4 hex chars from the MAC, plus null
static char s_ap_password[13] = "0000000000000"; // 12 hex chars, plus null

// FNV-1a 64-bit hash, used to derive a stable per-device AP password from the
// full MAC. Not cryptographic: the MAC is not secret, this only makes each unit
// unique instead of sharing one hardcoded password.
static uint64_t fnv1a64(const uint8_t *data, size_t len)
{
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 1099511628211ULL;
    }
    return h;
}

esp_err_t board_init(void)
{
    // Create the one I2C bus shared by the ES8311 codec and the FT6336G touch.
    // It is created exactly once here. Both drivers attach to this handle.
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BOARD_I2C_SDA_GPIO,
        .scl_io_num = BOARD_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c_bus), TAG,
                        "failed to create shared I2C bus");

    // Device ID: 4 hex chars from the last 2 bytes of the station MAC.
    uint8_t mac[6] = {0};
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_STA), TAG,
                        "failed to read MAC");
    snprintf(s_device_id, sizeof(s_device_id), "%02X%02X", mac[4], mac[5]);

    uint64_t h = fnv1a64(mac, sizeof(mac));
    snprintf(s_ap_password, sizeof(s_ap_password), "%012llX",
             (unsigned long long)(h & 0xFFFFFFFFFFFFULL));

    ESP_LOGI(TAG, "shared I2C bus ready (SDA=%d SCL=%d), device id %s",
             BOARD_I2C_SDA_GPIO, BOARD_I2C_SCL_GPIO, s_device_id);
    return ESP_OK;
}

i2c_master_bus_handle_t board_i2c_bus(void)
{
    return s_i2c_bus;
}

const char *board_device_id(void)
{
    return s_device_id;
}

const char *board_ap_password(void)
{
    return s_ap_password;
}
