idf_component_register(
    SRCS "playlist_m3u.c" "playlist_store.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES source_sd decode
)
