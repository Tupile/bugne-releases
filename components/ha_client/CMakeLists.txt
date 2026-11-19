idf_component_register(
    SRCS "ha_client.c" "ha_reply.c" "notif.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES config_store esp_http_client esp_system logstore mbedtls espressif__mdns lwip
)
