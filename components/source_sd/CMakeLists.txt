idf_component_register(
    SRCS "source_sd.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES audio decode board fatfs sdmmc esp_driver_sdmmc
)
