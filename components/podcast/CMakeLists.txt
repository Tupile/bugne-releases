idf_component_register(
    SRCS "podcast.c" "rss_parse.c" "yxml.c" "played.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."        # yxml.c / rss_parse.c do #include <yxml.h>
    PRIV_REQUIRES json esp_http_client esp-tls mbedtls esp_timer source_sd
)
