// board_pins: the fixed GPIO map of the LCDWIKI ES3C28P board.
//
// Single source of truth for pin numbers. Do not hardcode GPIO numbers
// elsewhere. See docs/hardware.md for the full board notes. These pins are
// fixed by the hardware and must not change.
#pragma once

// Display ILI9341V (SPI). Reset is tied to chip reset.
#define BOARD_LCD_CS_GPIO    10
#define BOARD_LCD_DC_GPIO    46
#define BOARD_LCD_SCLK_GPIO  12
#define BOARD_LCD_MOSI_GPIO  11
#define BOARD_LCD_MISO_GPIO  13
#define BOARD_LCD_BL_GPIO    45

// Shared I2C bus: ES8311 codec (0x18) and FT6336G touch (0x38).
#define BOARD_I2C_SDA_GPIO   16
#define BOARD_I2C_SCL_GPIO   15
#define BOARD_ES8311_ADDR    0x18
#define BOARD_FT6336G_ADDR   0x38

// Touch FT6336G control lines (besides the shared I2C).
#define BOARD_TOUCH_RST_GPIO 18
#define BOARD_TOUCH_INT_GPIO 17

// Audio amp FM8002E enable. Active low: low plays, high mutes.
#define BOARD_AMP_EN_GPIO    1

// I2S to the codec and from the mic. DOUT (ESP -> codec DSDIN) is on IO8 and
// DIN (codec ASDOUT -> ESP) is on IO6: confirmed against the working kidpod
// build on this exact board. They are the reverse of an earlier guess.
#define BOARD_I2S_MCLK_GPIO  4
#define BOARD_I2S_BCLK_GPIO  5
#define BOARD_I2S_DOUT_GPIO  8
#define BOARD_I2S_WS_GPIO    7
#define BOARD_I2S_DIN_GPIO   6

// microSD, SDIO 4-bit.
#define BOARD_SD_CLK_GPIO    38
#define BOARD_SD_CMD_GPIO    40
#define BOARD_SD_D0_GPIO     39
#define BOARD_SD_D1_GPIO     41
#define BOARD_SD_D2_GPIO     48
#define BOARD_SD_D3_GPIO     47

// RGB LED and the BOOT button (strapping pin, usable at runtime).
#define BOARD_RGB_LED_GPIO   42
#define BOARD_BOOT_BTN_GPIO  0

// Battery sense ADC pin: NOT confirmed on hardware, monitoring deferred.
// It is NOT IO1 (that is the amp enable). Do not use until verified.
