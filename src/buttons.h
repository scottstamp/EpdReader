#ifndef BUTTONS_H
#define BUTTONS_H

#include "config.h"

typedef enum {
    BTN_EVENT_NONE,
    BTN_EVENT_PREV_CLICK,
    BTN_EVENT_NEXT_CLICK,
    BTN_EVENT_SELECT_CLICK,
    BTN_EVENT_SELECT_LONG_PRESS
} ButtonEvent;

int buttons_init(void);
// Waits up to timeout_ms (-1 = forever) for the next button event; BTN_EVENT_NONE on timeout
ButtonEvent buttons_wait(int32_t timeout_ms);
// Removes and counts queued events equal to evt at the head of the queue (for batching page turns)
int buttons_take_repeats(ButtonEvent evt);
// Swap the Prev/Next pins' meaning; follows the 180-degree display orientation
void buttons_set_swapped(bool swapped);
void buttons_configure_sleep_wake(void);

#endif // BUTTONS_H
