// Host-test stub: i2c_new_master_bus hands back g_mock_i2c_bus_handle, or
// fails when g_mock_i2c_fail is set. Not the real header.
#pragma once
#include "esp_err.h"
#include <stdbool.h>

typedef int i2c_port_t;
#define I2C_NUM_0 0

typedef int i2c_clock_source_t;
#define I2C_CLK_SRC_DEFAULT 0

typedef void* i2c_master_bus_handle_t;

typedef struct {
    i2c_port_t i2c_port;
    int sda_io_num;
    int scl_io_num;
    i2c_clock_source_t clk_source;
    int glitch_ignore_cnt;
    struct {
        bool enable_internal_pullup;
    } flags;
} i2c_master_bus_config_t;

extern i2c_master_bus_handle_t g_mock_i2c_bus_handle;
extern bool g_mock_i2c_fail;

static inline esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *bus_config, i2c_master_bus_handle_t *ret_bus_handle) {
    (void)bus_config;
    if (g_mock_i2c_fail) return ESP_FAIL;
    if (ret_bus_handle) {
        *ret_bus_handle = g_mock_i2c_bus_handle;
    }
    return ESP_OK;
}
