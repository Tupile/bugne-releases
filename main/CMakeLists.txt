# The app entry point. REQUIRES lists the components the boot sequence calls
# directly. Each component pulls its own further dependencies.
idf_component_register(
    SRCS "bugne_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        app_update
        esp_pm
        logstore
        config_store
        board
        audio
        net
        web_config
        ui
        source_sd
        library
        source_stream
        source_sendspin
        podcast
        stats
        memo
)
