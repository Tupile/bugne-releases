idf_component_register(
    SRCS "source_stream.c" "rf_meta.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES audio decode esp_http_client esp-tls mbedtls
)
