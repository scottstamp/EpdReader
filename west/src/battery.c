#include "battery.h"
#include <zephyr/drivers/adc.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(battery, LOG_LEVEL_INF);

static const struct adc_dt_spec adc_channel = ADC_DT_SPEC_GET_BY_IDX(DT_PATH(vbatt), 0);

int battery_init(void) {
    if (!adc_is_ready_dt(&adc_channel)) {
        LOG_ERR("ADC device not ready");
        return -ENODEV;
    }

    int err = adc_channel_setup_dt(&adc_channel);
    if (err) {
        LOG_ERR("ADC channel setup failed (err %d)", err);
        return err;
    }

    LOG_INF("Battery ADC initialized.");
    return 0;
}

uint16_t battery_read_mv(void) {
    int16_t sample_buffer;
    struct adc_sequence sequence = {
        .buffer = &sample_buffer,
        .buffer_size = sizeof(sample_buffer),
    };

    int err = adc_sequence_init_dt(&adc_channel, &sequence);
    if (err) {
        return 3700;
    }

    err = adc_read_dt(&adc_channel, &sequence);
    if (err) {
        LOG_ERR("ADC read failed (err %d)", err);
        return 3700; // Default fallback (3.7V)
    }

    int32_t val_mv = sample_buffer;
    adc_raw_to_millivolts_dt(&adc_channel, &val_mv);

    // Apply 1/2 resistor divider multiplier (x2)
    return (uint16_t)(val_mv * 2);
}

uint8_t battery_get_percentage(void) {
    uint16_t mv = battery_read_mv();
    if (mv >= 4200) return 100;
    if (mv <= 3300) return 0;

    // Linear mapping between 3.3V (0%) and 4.2V (100%)
    return (uint8_t)(((mv - 3300) * 100) / (4200 - 3300));
}
