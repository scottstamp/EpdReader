cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(nrf_epd_zephyr)

zephyr_compile_definitions(CONFIG_NFCT_PINS_AS_GPIOS)

target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/display
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_sources(app PRIVATE
    src/main.c
    src/display.c
    drivers/display/display_gdey037t03.c
    src/storage.c
    src/buttons.c
    src/ble.c
    src/battery.c
    src/puff.c
    src/epub.c
)
