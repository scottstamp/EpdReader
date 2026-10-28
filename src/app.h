#ifndef APP_H
#define APP_H

#include <stdbool.h>
#include <stdint.h>

// Screens. The values are stored in the sleep-state file, so only append.
typedef enum {
    STATE_MENU,
    STATE_BOOK_LIST,
    STATE_CHAPTER_LIST,
    STATE_READER,
    STATE_READER_MENU,
    STATE_SETTINGS,
    STATE_REFRESH_SETTINGS,
    STATE_LOCKSCREEN,
    STATE_STATS,
    STATE_BLE_UPLOAD,
    STATE_USB_STORAGE,
    STATE_TRANSFER,
    STATE_LISTS,
    STATE_LIST_ITEMS,
    STATE_COUNT
} SystemState;

// One per SystemState. Every hook but render is optional.
typedef struct {
    void (*enter)(SystemState from); // on boot too, as if coming from STATE_MENU
    void (*leave)(SystemState to);   // every way off the screen passes here: release what enter took
    void (*render)(void);
    void (*step)(int delta);         // Prev (-1) / Next (+1)
    void (*select)(void);
    void (*poll)(void);              // after every wake-up while the screen is shown
    int32_t poll_ms;                 // wake at least this often to poll (0: only on events)
    bool long_press_sleeps;          // top-level screens: long press sleeps instead of going home
    bool stay_awake;                 // no auto-sleep (a computer holds the SD card)
} Screen;

SystemState app_state(void);

// Runs the leave/enter hooks when the screen changes, then draws it
void app_go(SystemState next);
void app_redraw(void);

// Lock screen, then System OFF; doesn't return
void app_sleep(void);

// Restarts the auto-sleep timer
void app_keep_awake(void);

// Shows `initial` and handles buttons from then on; doesn't return
void app_run(SystemState initial);

static inline int wrap_index(int idx, int delta, int count) {
    return (idx + delta + count) % count;
}

#endif // APP_H
