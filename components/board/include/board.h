// board: pin map, the single shared I2C bus, and the device identity.
//
// Owns the one i2c_master_bus shared by the ES8311 codec (0x18) and the
// FT6336G touch (0x38). Also owns the RGB LED, the BOOT button, and the
// battery ADC (pin to confirm on hardware).
#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bring up board-level peripherals: create the shared I2C bus and compute the
// device ID from the MAC.
esp_err_t board_init(void);

// The one shared I2C bus handle. Pass it to both the codec and the touch
// driver. Never create a second bus.
i2c_master_bus_handle_t board_i2c_bus(void);

// 4 hex chars from the last 2 bytes of the MAC, for example "A1B2". Drives the
// mDNS hostname, the setup AP SSID, the AP password seed, and the Sendspin name.
const char *board_device_id(void);

// Per-device WPA2 password for the setup AP, derived from a hash of the full
// MAC (not a hardcoded shared secret). Stable across reboots, 12 hex chars.
const char *board_ap_password(void);

// Human-readable reason for the last chip reset ("POWERON", "PANIC", ...).
// Logged at boot and served in /api/status so an unexpected reboot is
// classifiable remotely (the PSRAM log ring does not survive a reset).
const char *board_reset_reason_name(void);

#ifdef __cplusplus
}
#endif
