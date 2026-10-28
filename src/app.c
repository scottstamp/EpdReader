// Screen state machine and the button loop
#include <zephyr/kernel.h>
#include "app.h"
#include "book.h"
#include "buttons.h"
#include "config.h"
#include "screens.h"

static const Screen *const screens[STATE_COUNT] = {
    [STATE_MENU] = &screen_home,
    [STATE_BOOK_LIST] = &screen_library,
    [STATE_CHAPTER_LIST] = &screen_chapters,
    [STATE_READER] = &screen_reader,
    [STATE_READER_MENU] = &screen_reader_menu,
    [STATE_SETTINGS] = &screen_settings,
    [STATE_REFRESH_SETTINGS] = &screen_refresh_settings,
    [STATE_LOCKSCREEN] = &screen_lockscreen,
    [STATE_STATS] = &screen_stats,
    [STATE_BLE_UPLOAD] = &screen_ble_upload,
    [STATE_USB_STORAGE] = &screen_usb_storage,
    [STATE_TRANSFER] = &screen_transfer,
    [STATE_LISTS] = &screen_lists,
    [STATE_LIST_ITEMS] = &screen_list_items,
};

static SystemState current = STATE_MENU;
static uint32_t last_activity_ms;

SystemState app_state(void) {
    return current;
}

void app_redraw(void) {
    screens[current]->render();
}

void app_go(SystemState next) {
    if (next != current) {
        SystemState from = current;
        if (screens[from]->leave) screens[from]->leave(next);
        current = next;
        if (screens[next]->enter) screens[next]->enter(from);
    }
    app_redraw();
}

void app_sleep(void) {
    app_go(STATE_LOCKSCREEN);
}

void app_keep_awake(void) {
    last_activity_ms = k_uptime_get_32();
}

// Earliest of two timeouts, where -1 means none
static int32_t sooner(int32_t a, int32_t b) {
    if (a < 0) return b;
    if (b < 0) return a;
    return MIN(a, b);
}

void app_run(SystemState initial) {
    current = initial;
    if (screens[current]->enter) screens[current]->enter(STATE_MENU);
    app_redraw();
    app_keep_awake();

    while (1) {
        // Sleep until a button event, the pending progress save, a poll, or the auto-sleep deadline
        const Screen *s = screens[current];
        int32_t timeout_ms = -1;
        if (!s->stay_awake) {
            uint32_t idle = k_uptime_get_32() - last_activity_ms;
            timeout_ms = (idle >= AUTO_SLEEP_MS) ? 0 : (int32_t)(AUTO_SLEEP_MS - idle);
        }
        timeout_ms = sooner(timeout_ms, book_save_due_in_ms());
        if (s->poll_ms > 0) {
            timeout_ms = sooner(timeout_ms, s->poll_ms);
        }

        ButtonEvent evt = buttons_wait(timeout_ms);
        if (evt != BTN_EVENT_NONE) {
            app_keep_awake();
        }

        switch (evt) {
            case BTN_EVENT_PREV_CLICK:
            case BTN_EVENT_NEXT_CLICK:
                if (s->step) s->step(evt == BTN_EVENT_NEXT_CLICK ? 1 : -1);
                break;
            case BTN_EVENT_SELECT_CLICK:
                if (s->select) s->select();
                break;
            case BTN_EVENT_SELECT_LONG_PRESS:
                // Back to the home carousel from anywhere below it; sleep from the top level
                if (s->long_press_sleeps) {
                    app_sleep();
                } else {
                    app_go(STATE_MENU);
                }
                break;
            default:
                break;
        }

        book_service();

        s = screens[current];
        if (s->poll) {
            s->poll();
        }

        if (screens[current]->stay_awake) {
            app_keep_awake();
        } else if (k_uptime_get_32() - last_activity_ms >= AUTO_SLEEP_MS) {
            app_sleep();
        }
    }
}
