idf_component_register(
    SRCS "web_config.c"
    INCLUDE_DIRS "include"
    EMBED_FILES "www/index.html" "www/login.html"
    PRIV_REQUIRES esp_http_server json esp_system esp_hw_support app_update config_store board net logstore ui source_sd library esp_https_ota mbedtls esp_timer esp_wifi
)
