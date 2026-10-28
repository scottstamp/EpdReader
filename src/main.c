#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <hal/nrf_power.h>
#include "app.h"
#include "battery.h"
#include "book.h"
#include "buttons.h"
#include "display.h"
#include "screens.h"
#include "storage.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

// The screen to start on: the one shown before sleep, else the home carousel with the
// last opened book ready to continue
static SystemState restore_session(void) {
    int state = -1;
    int home_idx = 0;
    char book[64] = {0};
    uint32_t offset = 0;

    if (!storage_read_sleep_state(&state, &home_idx, book) || state < 0) {
        if (storage_read_progress(book, &offset) && book[0] != '\0') {
            book_restore(book, offset);
        }
        return STATE_MENU;
    }
    storage_clear_sleep_state();

    if (book[0] != '\0') {
        storage_read_book_progress(book, &offset);
        book_restore(book, offset);
    }
    switch (state) {
        case STATE_READER:
        case STATE_READER_MENU:
            return STATE_READER;
        case STATE_BOOK_LIST:
            return STATE_BOOK_LIST;
        default:
            home_set_selection(home_idx);
            return STATE_MENU;
    }
}

int main(void) {
    // Clear the "boot into the UF2 bootloader" request in case it was left set
    nrf_power_gpregret_set(NRF_POWER, 0, 0);

    usb_init();
    LOG_INF("Starting nRF e-Paper Book Reader (Zephyr RTOS)");

    // Mount the SD card first: display_init loads the SD font from it
    storage_init();
    display_init();
    book_init();
    buttons_init();
    battery_init();
    user_settings_load();

    app_run(restore_session());
    return 0;
}
