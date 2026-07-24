idf_component_register(
    SRCS "source_sendspin.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES audio board net config_store esp_timer esp_app_format espressif__mdns sendspin__sendspin-cpp
)
