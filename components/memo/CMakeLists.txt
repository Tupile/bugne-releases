idf_component_register(
    SRCS "memo_wav.c" "memo_name.c" "memo_store.c" "memo_send.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_http_client source_sd
)
