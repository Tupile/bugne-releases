// button_pins: allowed button GPIOs and action names. See button_pins.h.
#include "button_pins.h"

#include <stddef.h>
#include <string.h>

static const char *const ACTION_NAMES[BUTTON_ACT_COUNT] = {
    [BUTTON_ACT_TOGGLE]   = "toggle",
    [BUTTON_ACT_VOL_UP]   = "vol_up",
    [BUTTON_ACT_VOL_DOWN] = "vol_down",
};

button_pin_class_t button_pin_class(int gpio)
{
    switch (gpio) {
    case 2: case 14: case 21: case 44:
        return BUTTON_PIN_FREE;
    case 0: case 3: case 43:
        return BUTTON_PIN_CAUTION;
    default:
        return BUTTON_PIN_FORBIDDEN;
    }
}

int button_action_from_name(const char *name)
{
    if (!name) return -1;
    for (int i = 0; i < BUTTON_ACT_COUNT; i++)
        if (strcmp(name, ACTION_NAMES[i]) == 0) return i;
    return -1;
}

const char *button_action_name(int action)
{
    return (action >= 0 && action < BUTTON_ACT_COUNT) ? ACTION_NAMES[action] : "";
}
