// Home carousel and reading stats
#include <zephyr/kernel.h>
#include <stdio.h>
#include "book.h"
#include "display.h"
#include "screens.h"
#include "storage.h"

// Carousel entries, in display order
typedef enum {
    HOME_CONTINUE,
    HOME_LIBRARY,
    HOME_LISTS,
    HOME_SETTINGS,
    HOME_TRANSFER,
    HOME_STATS,
    HOME_SLEEP,
    HOME_COUNT
} HomeItem;

static int selected = HOME_CONTINUE;

int home_selection(void) {
    return selected;
}

void home_set_selection(int idx) {
    selected = (idx >= 0 && idx < HOME_COUNT) ? idx : 0;
}

static void home_render(void) {
    char continue_detail[80] = "No book opened yet";
    if (book_is_open()) {
        char title[64];
        storage_get_book_display_title(book_name(), title, sizeof(title));
        snprintf(continue_detail, sizeof(continue_detail), "%s - %u%%", title, book_progress_percent());
    }

    uint32_t sec = reading_total_seconds();
    char stats_detail[40];
    snprintf(stats_detail, sizeof(stats_detail), "%uh %02um read in total", sec / 3600, (sec / 60) % 60);

    const CarouselItem items[HOME_COUNT] = {
        [HOME_CONTINUE] = { &icon_continue_large, &icon_continue_small, "Continue", continue_detail },
        [HOME_LIBRARY]  = { &icon_library_large, &icon_library_small, "Library", "Books on the SD card" },
        [HOME_LISTS]    = { &icon_lists_large, &icon_lists_small, "Lists", "To-do and grocery lists" },
        [HOME_SETTINGS] = { &icon_settings_large, &icon_settings_small, "Settings", "Text, display, firmware" },
        [HOME_TRANSFER] = { &icon_transfer_large, &icon_transfer_small, "Transfer", "USB drive or Bluetooth" },
        [HOME_STATS]    = { &icon_stats_large, &icon_stats_small, "Stats", stats_detail },
        [HOME_SLEEP]    = { &icon_sleep_large, &icon_sleep_small, "Sleep", "Lock screen and power off" },
    };
    display_draw_carousel(items, HOME_COUNT, selected);
}

static void home_step(int delta) {
    selected = wrap_index(selected, delta, HOME_COUNT);
    app_redraw();
}

static void home_select(void) {
    switch ((HomeItem)selected) {
        case HOME_CONTINUE:
            if (book_is_open()) {
                app_go(STATE_READER);
            } else {
                display_draw_message("Continue", "No book opened yet. Pick one in the Library.", true);
                k_msleep(1500);
                app_redraw();
            }
            break;
        case HOME_LIBRARY:  app_go(STATE_BOOK_LIST); break;
        case HOME_LISTS:    app_go(STATE_LISTS); break;
        case HOME_SETTINGS: app_go(STATE_SETTINGS); break;
        case HOME_TRANSFER: app_go(STATE_TRANSFER); break;
        case HOME_STATS:    app_go(STATE_STATS); break;
        case HOME_SLEEP:    app_sleep(); break;
        default: break;
    }
}

const Screen screen_home = {
    .render = home_render,
    .step = home_step,
    .select = home_select,
    .long_press_sleeps = true,
};

static void stats_render(void) {
    uint32_t sec = reading_total_seconds();
    char msg[64];
    snprintf(msg, sizeof(msg), "Total reading time: %uh %02um", sec / 3600, (sec / 60) % 60);
    display_draw_message("Reading Stats", msg, false);
}

static void back_home(void) {
    app_go(STATE_MENU);
}

const Screen screen_stats = {
    .render = stats_render,
    .select = back_home,
    .long_press_sleeps = true,
};
