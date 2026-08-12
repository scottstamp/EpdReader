#ifndef BUTTONS_H
#define BUTTONS_H

#include "config.h"

typedef enum {
    BTN_EVENT_NONE,
    BTN_EVENT_PREV_CLICK,
    BTN_EVENT_NEXT_CLICK,
    BTN_EVENT_SELECT_CLICK,
    BTN_EVENT_SELECT_DOUBLE_CLICK,
    BTN_EVENT_SELECT_LONG_PRESS
} ButtonEvent;

int buttons_init(void);
ButtonEvent buttons_poll(void);
void buttons_configure_sleep_wake(void);

#endif // BUTTONS_H
