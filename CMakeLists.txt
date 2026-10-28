cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(nrf_epd_zephyr)

target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/display
    ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/tjpgd
)

target_sources(app PRIVATE
    src/main.c
    src/app.c
    src/book.c
    src/screen_home.c
    src/screen_library.c
    src/screen_reader.c
    src/screen_settings.c
    src/screen_lists.c
    src/screen_transfer.c
    src/screen_sleep.c
    src/display.c
    drivers/display/display_gdey037t03.c
    src/storage.c
    src/buttons.c
    src/ble.c
    src/ble_upload.c
    src/upload_proto.c
    src/battery.c
    src/prefs.c
    src/lists_doc.c
    src/lists.c
    src/lists_ble.c
    src/inflate_stream.c
    src/dither.c
    src/html_text.c
    src/epub.c
    src/font_extended.c
    src/third_party/tjpgd/tjpgd.c
)
