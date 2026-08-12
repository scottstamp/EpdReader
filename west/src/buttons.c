#include "buttons.h"
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <hal/nrf_gpio.h>

LOG_MODULE_REGISTER(buttons, LOG_LEVEL_INF);

static const struct gpio_dt_spec btn_prev   = GPIO_DT_SPEC_GET_OR(DT_ALIAS(btn_prev), gpios, {0});
static const struct gpio_dt_spec btn_next   = GPIO_DT_SPEC_GET_OR(DT_ALIAS(btn_next), gpios, {0});
static const struct gpio_dt_spec btn_select = GPIO_DT_SPEC_GET_OR(DT_ALIAS(btn_select), gpios, {0});

static struct gpio_callback cb_prev;
static struct gpio_callback cb_next;
static struct gpio_callback cb_select;

static volatile ButtonEvent pending_event = BTN_EVENT_NONE;
static uint32_t last_prev_press_time = 0;
static uint32_t last_next_press_time = 0;
static uint32_t last_select_press_time = 0;

static void button_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins) {
    uint32_t now = k_uptime_get_32();

    if (pins & BIT(btn_prev.pin)) {
        if (now - last_prev_press_time >= 350) {
            last_prev_press_time = now;
            pending_event = BTN_EVENT_PREV_CLICK;
        }
    } else if (pins & BIT(btn_next.pin)) {
        if (now - last_next_press_time >= 350) {
            last_next_press_time = now;
            pending_event = BTN_EVENT_NEXT_CLICK;
        }
    } else if (pins & BIT(btn_select.pin)) {
        if (now - last_select_press_time >= 350) {
            if (last_select_press_time > 0 && (now - last_select_press_time < 600)) {
                pending_event = BTN_EVENT_SELECT_DOUBLE_CLICK;
                last_select_press_time = 0;
            } else {
                pending_event = BTN_EVENT_SELECT_CLICK;
                last_select_press_time = now;
            }
        }
    }
}

int buttons_init(void) {
    last_prev_press_time = 0;
    last_next_press_time = 0;
    last_select_press_time = 0;

    if (btn_prev.port) {
        gpio_pin_configure_dt(&btn_prev, GPIO_INPUT | GPIO_PULL_UP);
        gpio_pin_interrupt_configure_dt(&btn_prev, GPIO_INT_EDGE_TO_ACTIVE);
        gpio_init_callback(&cb_prev, button_isr, BIT(btn_prev.pin));
        gpio_add_callback(btn_prev.port, &cb_prev);
    }

    if (btn_next.port) {
        gpio_pin_configure_dt(&btn_next, GPIO_INPUT | GPIO_PULL_UP);
        gpio_pin_interrupt_configure_dt(&btn_next, GPIO_INT_EDGE_TO_ACTIVE);
        gpio_init_callback(&cb_next, button_isr, BIT(btn_next.pin));
        gpio_add_callback(btn_next.port, &cb_next);
    }

    if (btn_select.port) {
        gpio_pin_configure_dt(&btn_select, GPIO_INPUT | GPIO_PULL_UP);
        gpio_pin_interrupt_configure_dt(&btn_select, GPIO_INT_EDGE_TO_ACTIVE);
        gpio_init_callback(&cb_select, button_isr, BIT(btn_select.pin));
        gpio_add_callback(btn_select.port, &cb_select);
    }

    LOG_INF("Buttons subsystem initialized with 350ms per-pin debounce.");
    return 0;
}

ButtonEvent buttons_poll(void) {
    // Check for long press on select button
    if (btn_select.port && gpio_pin_get_dt(&btn_select) == 1) { // Pressed (ACTIVE_LOW)
        if (last_select_press_time > 0 && (k_uptime_get_32() - last_select_press_time > 5000)) {
            last_select_press_time = 0;
            return BTN_EVENT_SELECT_LONG_PRESS;
        }
    }

    ButtonEvent evt = pending_event;
    pending_event = BTN_EVENT_NONE;
    return evt;
}

void buttons_configure_sleep_wake(void) {
    // Configure nRF GPIO Sense Low for deep sleep wake-up on button press
    nrf_gpio_cfg_sense_input(NRF_GPIO_PIN_MAP(0, 2), NRF_GPIO_PIN_PULLUP, NRF_GPIO_PIN_SENSE_LOW);  // Prev (P0.02)
    nrf_gpio_cfg_sense_input(NRF_GPIO_PIN_MAP(0, 24), NRF_GPIO_PIN_PULLUP, NRF_GPIO_PIN_SENSE_LOW); // Next (P0.24)
    nrf_gpio_cfg_sense_input(NRF_GPIO_PIN_MAP(0, 22), NRF_GPIO_PIN_PULLUP, NRF_GPIO_PIN_SENSE_LOW); // Select (P0.22)
    LOG_INF("GPIO sense wake configured for deep sleep.");
}
