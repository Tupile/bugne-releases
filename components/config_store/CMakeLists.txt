idf_component_register(
    SRCS "config_store.c"
    INCLUDE_DIRS "include"
    # The managed component joltwallet/littlefs registers as joltwallet__littlefs.
    # If the first on-device build reports it missing, try the name "littlefs".
    REQUIRES nvs_flash json joltwallet__littlefs podcast
    PRIV_REQUIRES mbedtls esp_hw_support
)
