# ui drives the display and touch. Public header exposes the i2c_master type.
idf_component_register(
    SRCS "ui.c" "lang.c" "quiet.c" "usage.c" "alarm_next.c" "fonts/bugne_font_14.c" "fonts/bugne_font_20.c" "fonts/bugne_font_48.c" "lv_mem_psram.c"
    INCLUDE_DIRS "include"
    REQUIRES driver
    PRIV_REQUIRES esp_lcd esp_lcd_touch esp_driver_spi esp_timer esp_app_format
                  espressif__esp_lcd_ili9341 espressif__esp_lcd_touch_ft5x06
                  espressif__esp_lvgl_port lvgl__lvgl
                  board net config_store source_sd source_stream source_sendspin podcast audio decode library stats tuner memo
    WHOLE_ARCHIVE  # keep lv_mem_psram.c: LVGL (a dependency) calls lv_*_core but
                   # nothing here references them, so the linker would drop them.
)
