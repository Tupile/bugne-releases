// button_pins: which ESP32-S3 GPIOs may carry an external button (config
// "buttons"), and the button action names. Pure C, no ESP-IDF, host-tested.
//
// Board LCDWIKI ES3C28P (see .sage-memory/bugne-hardware-map.md): most pins
// drive the display, touch, codec, SD or flash/PSRAM; configuring one of them
// as an input would break that function (flash/PSRAM pins crash the boot, and
// the config survives the reboot). Only the pins below are accepted:
//   free:    IO2, IO14, IO21, IO44 (UART0 RX on the UART header)
//   caution: IO0 (BOOT button, also wakes the screen), IO3 (strapping pin),
//            IO43 (UART0 TX: drives high during boot, so a press at that
//            moment shorts it; prefer RX)
// Using IO43/IO44 detaches them from the UART0 console until the next reboot;
// logs stay available on the USB port. The web page holds a copy of this table
// with each busy pin's role: keep both in sync.
#pragma once

#include <stdbool.h>

typedef enum {
    BUTTON_PIN_FORBIDDEN = 0,  // used by the board, or not routed
    BUTTON_PIN_FREE,
    BUTTON_PIN_CAUTION,        // allowed, with a warning on the web page
} button_pin_class_t;

typedef enum {
    BUTTON_ACT_TOGGLE = 0,  // play / pause
    BUTTON_ACT_VOL_UP,      // volume + one step, repeats while held
    BUTTON_ACT_VOL_DOWN,    // volume - one step, repeats while held
    BUTTON_ACT_COUNT
} button_action_t;

button_pin_class_t button_pin_class(int gpio);
static inline bool button_pin_allowed(int gpio) { return button_pin_class(gpio) != BUTTON_PIN_FORBIDDEN; }

// Config name of an action ("toggle", "vol_up", "vol_down"); -1 if unknown.
int button_action_from_name(const char *name);
const char *button_action_name(int action);  // "" if out of range
