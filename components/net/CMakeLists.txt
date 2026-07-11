idf_component_register(
    SRCS "net.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_wifi esp_netif esp_event esp_timer espressif__mdns lwip config_store board esp_app_format
)
