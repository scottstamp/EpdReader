#include "buttons.h"
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <hal/nrf_gpio.h>

LOG_MODULE_REGISTER(buttons, LOG_LEVEL_INF);

#define DEBOUNCE_MS 350
#define LONG_PRESS_MS 1000
#define LONG_PRESS_POLL_MS 50
// A Select release only counts once the pin has stayed released this long (contact bounce)
#define RELEASE_SETTLE_MS 30

static const struct gpio_dt_spec btn_prev   = GPIO_DT_SPEC_GET_OR(DT_ALIAS(btn_prev), gpios, {0});
static const struct gpio_dt_spec btn_next   = GPIO_DT_SPEC_GET_OR(DT_ALIAS(btn_next), gpios, {0});
static const struct gpio_dt_spec btn_select = GPIO_DT_SPEC_GET_OR(DT_ALIAS(btn_select), gpios, {0});

static struct gpio_callback cb_prev;
static struct gpio_callback cb_next;
static struct gpio_callback cb_select;

// Presses made while the panel refreshes are queued rather than dropped
K_MSGQ_DEFINE(button_events, sizeof(ButtonEvent), 8, 4);

static uint32_t last_prev_press_time = 0;
static uint32_t last_next_press_time = 0;
static uint32_t last_select_press_time = 0;
// Select reports a click on release, or a long press once held for LONG_PRESS_MS,
// so a hold never triggers the click action first
static volatile uint32_t select_hold_start = 0; // 0 while Select is up
static volatile bool select_long_fired = false;
static volatile bool swapped = true;

static void queue_event(ButtonEvent evt) {
    k_msgq_put(&button_events, &evt, K_NO_WAIT);
}

static void select_release_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    if (gpio_pin_get_dt(&btn_select) == 1) {
        return; // bounce: still pressed
    }
    if (select_hold_start != 0 && !select_long_fired) {
        queue_event(BTN_EVENT_SELECT_CLICK);
    }
    select_hold_start = 0;
    select_long_fired = false;
}

static K_WORK_DELAYABLE_DEFINE(select_release_work, select_release_work_handler);

static void button_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins) {
    uint32_t now = k_uptime_get_32();

    if (pins & BIT(btn_prev.pin)) {
        if (now - last_prev_press_time >= DEBOUNCE_MS) {
            last_prev_press_time = now;
            queue_event(swapped ? BTN_EVENT_NEXT_CLICK : BTN_EVENT_PREV_CLICK);
        }
    } else if (pins & BIT(btn_next.pin)) {
        if (now - last_next_press_time >= DEBOUNCE_MS) {
            last_next_press_time = now;
            queue_event(swapped ? BTN_EVENT_PREV_CLICK : BTN_EVENT_NEXT_CLICK);
        }
    } else if (pins & BIT(btn_select.pin)) {
        if (gpio_pin_get_dt(&btn_select) == 1) {
            if (select_hold_start == 0 && now - last_select_press_time >= DEBOUNCE_MS) {
                last_select_press_time = now;
                select_hold_start = now ? now : 1;
                select_long_fired = false;
                // Wake buttons_wait so it starts polling for the long press
                queue_event(BTN_EVENT_NONE);
            }
        } else if (select_hold_start != 0) {
            k_work_reschedule(&select_release_work, K_MSEC(RELEASE_SETTLE_MS));
        }
    }
}

static void setup_button(const struct gpio_dt_spec *btn, struct gpio_callback *cb, gpio_flags_t edges) {
    if (!btn->port) return;
    gpio_pin_configure_dt(btn, GPIO_INPUT | GPIO_PULL_UP);
    gpio_pin_interrupt_configure_dt(btn, edges);
    gpio_init_callback(cb, button_isr, BIT(btn->pin));
    gpio_add_callback(btn->port, cb);
}

int buttons_init(void) {
    last_prev_press_time = 0;
    last_next_press_time = 0;
    last_select_press_time = 0;

    setup_button(&btn_prev, &cb_prev, GPIO_INT_EDGE_TO_ACTIVE);
    setup_button(&btn_next, &cb_next, GPIO_INT_EDGE_TO_ACTIVE);
    setup_button(&btn_select, &cb_select, GPIO_INT_EDGE_BOTH); // release decides click vs hold

    LOG_INF("Buttons subsystem initialized with %dms per-pin debounce.", DEBOUNCE_MS);
    return 0;
}

void buttons_set_swapped(bool swap) {
    swapped = swap;
}

static bool select_held(void) {
    return select_hold_start != 0 && !select_long_fired && btn_select.port && gpio_pin_get_dt(&btn_select) == 1;
}

static ButtonEvent check_long_press(void) {
    if (select_held() && (k_uptime_get_32() - select_hold_start >= LONG_PRESS_MS)) {
        select_long_fired = true; // the release that follows is not a click
        return BTN_EVENT_SELECT_LONG_PRESS;
    }
    return BTN_EVENT_NONE;
}

ButtonEvent buttons_wait(int32_t timeout_ms) {
    int64_t deadline = (timeout_ms < 0) ? -1 : k_uptime_get() + timeout_ms;

    while (true) {
        // Recover if a release edge was ever missed, so Select can't stay "held"
        if (select_hold_start != 0 && btn_select.port && gpio_pin_get_dt(&btn_select) == 0 &&
            !k_work_delayable_is_pending(&select_release_work)) {
            k_work_reschedule(&select_release_work, K_MSEC(RELEASE_SETTLE_MS));
        }

        ButtonEvent evt = check_long_press();
        if (evt != BTN_EVENT_NONE) return evt;

        int64_t wait_ms = -1;
        if (deadline >= 0) {
            wait_ms = deadline - k_uptime_get();
            if (wait_ms <= 0) return BTN_EVENT_NONE;
        }
        // While Select is held, wake periodically to detect the long press
        if (select_held() && (wait_ms < 0 || wait_ms > LONG_PRESS_POLL_MS)) {
            wait_ms = LONG_PRESS_POLL_MS;
        }

        k_timeout_t timeout = (wait_ms < 0) ? K_FOREVER : K_MSEC(wait_ms);
        if (k_msgq_get(&button_events, &evt, timeout) == 0 && evt != BTN_EVENT_NONE) {
            return evt;
        }
        // Timed out or woken by a Select press: loop to re-check the hold and deadline
    }
}

int buttons_take_repeats(ButtonEvent evt) {
    int count = 0;
    ButtonEvent next;
    while (k_msgq_peek(&button_events, &next) == 0 && (next == evt || next == BTN_EVENT_NONE)) {
        k_msgq_get(&button_events, &next, K_NO_WAIT);
        if (next == evt) count++; // NONE entries are wake-ups from a Select press
    }
    return count;
}

void buttons_configure_sleep_wake(void) {
    // Configure nRF GPIO Sense Low for deep sleep wake-up on button press
    nrf_gpio_cfg_sense_input(NRF_GPIO_PIN_MAP(0, 2), NRF_GPIO_PIN_PULLUP, NRF_GPIO_PIN_SENSE_LOW);  // Prev (P0.02)
    nrf_gpio_cfg_sense_input(NRF_GPIO_PIN_MAP(0, 24), NRF_GPIO_PIN_PULLUP, NRF_GPIO_PIN_SENSE_LOW); // Next (P0.24)
    nrf_gpio_cfg_sense_input(NRF_GPIO_PIN_MAP(0, 22), NRF_GPIO_PIN_PULLUP, NRF_GPIO_PIN_SENSE_LOW); // Select (P0.22)
    LOG_INF("GPIO sense wake configured for deep sleep.");
}
