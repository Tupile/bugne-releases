idf_component_register(
    SRCS "decode.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES audio espressif__esp_audio_codec
)
