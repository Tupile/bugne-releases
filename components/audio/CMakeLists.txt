# audio owns I2S + amp + arbiter. Public header exposes the i2c_master type.
idf_component_register(
    SRCS "audio.c" "audio_arbiter.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c
    PRIV_REQUIRES esp_driver_i2s esp_driver_gpio esp_codec_dev board esp_timer
)
