// Lock screen and System OFF
#include <zephyr/kernel.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include "book.h"
#include "buttons.h"
#include "display.h"
#include "screens.h"
#include "storage.h"

LOG_MODULE_REGISTER(sleep, LOG_LEVEL_INF);

// The screen to bring back on wake-up
static SystemState pre_sleep_state = STATE_MENU;

static void lockscreen_enter(SystemState from) {
    pre_sleep_state = from;
}

static void enter_deep_sleep(void) {
    LOG_INF("Entering System OFF deep sleep...");
    book_flush_progress();
    user_settings_save();
    display_power_off();
    buttons_configure_sleep_wake();
    k_msleep(100);
    sys_poweroff();
}

// Draws the lock screen and powers off; a button press boots the device again
static void lockscreen_render(void) {
    char title[64] = {0};
    char author[64] = {0};
    if (book_is_open()) {
        storage_get_book_metadata(book_name(), title, sizeof(title), author, sizeof(author));
    } else {
        snprintf(title, sizeof(title), "e-Paper Reader");
        snprintf(author, sizeof(author), "nRF52840 Zephyr");
    }

    char chapter[64];
    book_chapter_title(book_chapter_index(), chapter, sizeof(chapter));

    display_draw_lockscreen(book_name(), title, author, chapter, book_progress_percent());
    storage_write_sleep_state((int)pre_sleep_state, home_selection(), book_name());
    enter_deep_sleep();
}

const Screen screen_lockscreen = {
    .enter = lockscreen_enter,
    .render = lockscreen_render,
};
