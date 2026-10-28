#ifndef SCREENS_H
#define SCREENS_H

#include "app.h"

// Screen tables, wired to their states in app.c
extern const Screen screen_home;
extern const Screen screen_stats;
extern const Screen screen_library;
extern const Screen screen_reader;
extern const Screen screen_reader_menu;
extern const Screen screen_chapters;
extern const Screen screen_settings;
extern const Screen screen_refresh_settings;
extern const Screen screen_lockscreen;
extern const Screen screen_transfer;
extern const Screen screen_ble_upload;
extern const Screen screen_usb_storage;
extern const Screen screen_lists;
extern const Screen screen_list_items;

// Home carousel position (kept across sleep)
int home_selection(void);
void home_set_selection(int idx);

// Display settings and reading stats, persisted in NVS
void user_settings_load(void);
void user_settings_save(void);

// USB device with the serial console; the SD card is only added in USB drive mode
void usb_init(void);

#endif // SCREENS_H
