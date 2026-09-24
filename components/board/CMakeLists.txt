# board owns the shared I2C bus. Its public header exposes the i2c_master type,
# so REQUIRES esp_driver_i2c publicly. esp_hw_support is only used internally (MAC read).
idf_component_register(
    SRCS "board.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c
    PRIV_REQUIRES esp_driver_gpio esp_hw_support
    # Future: esp_adc (battery sense, deferred), led_strip (RGB LED).
)
