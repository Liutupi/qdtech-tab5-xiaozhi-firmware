idf_component_register(SRCS "updater_main.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_driver_gpio esp_driver_sdmmc sdmmc fatfs bootloader_support
                                     app_update esp_partition esp_app_format mbedtls esp_system)
