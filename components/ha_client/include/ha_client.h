#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Toggle the configured Home Assistant light.
 */
void ha_client_toggle_light(void);

/**
 * @brief Set the color and brightness of the configured Home Assistant light.
 * 
 * @param r Red (0-255)
 * @param g Green (0-255)
 * @param b Blue (0-255)
 * @param brightness Brightness (0-255)
 */
void ha_client_set_light_color(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness);

#ifdef __cplusplus
}
#endif
