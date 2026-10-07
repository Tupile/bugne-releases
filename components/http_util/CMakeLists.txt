idf_component_register(
    SRCS "url_escape.c" "http_util.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client
)
