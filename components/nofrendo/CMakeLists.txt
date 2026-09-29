file(GLOB NOFRENDO_SOURCES
    "*.c"
    "cpu/*.c"
    "mappers/*.c"
    "nes/*.c"
    "sndhrdw/*.c"
    "libsnss/*.c"
)

list(REMOVE_ITEM NOFRENDO_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/qdtech_osd.c")
list(REMOVE_ITEM NOFRENDO_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/config.c")
list(REMOVE_ITEM NOFRENDO_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/memguard.c")
list(APPEND NOFRENDO_SOURCES "qdtech_osd.c")

idf_component_register(
    SRCS ${NOFRENDO_SOURCES}
    INCLUDE_DIRS
        "."
        "cpu"
        "mappers"
        "nes"
        "sndhrdw"
        "libsnss"
    REQUIRES esp_timer freertos
)

# The project builds with -Os; the 6502/PPU/APU inner loops need -O2 to hold 60 fps.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -O2
    -DQD_NOFRENDO_NO_GUI_MSG=1
    -Wno-error
    -Wno-int-to-pointer-cast
    -Wno-pointer-to-int-cast
    -Wno-incompatible-pointer-types
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-format
    -Wno-error=attributes
    -Wno-error=implicit-fallthrough
    -Wno-error=maybe-uninitialized
    -Wno-error=stringop-truncation
)
