set(TAB5_BOARD ${CMAKE_CURRENT_LIST_DIR}/../../main/boards/qdtech/tab5)
idf_component_register(SRCS "updater_main.c" "md_main.c" "md_board.c" "md_video.c"
                            "${TAB5_BOARD}/esp_lcd_st7121.c"
                       INCLUDE_DIRS "." "${TAB5_BOARD}"
                       PRIV_REQUIRES esp_driver_gpio esp_driver_sdmmc sdmmc fatfs bootloader_support
                                     app_update esp_partition esp_app_format mbedtls esp_system
                                     esp_lcd esp_driver_ppa esp_driver_i2c esp_driver_ledc esp_mm esp_timer heap
                                     gwenesis)

# Development only: `idf.py -DMD_TEST_ROM=/sdcard/roms/md/x.bin build` makes every boot of this
# app (e.g. after `otatool.py switch_ota_partition --slot 0`) run that ROM without a launch file.
if(DEFINED MD_TEST_ROM)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MD_TEST_ROM="${MD_TEST_ROM}")
endif()
