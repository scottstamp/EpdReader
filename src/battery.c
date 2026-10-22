#include "battery.h"
#include <zephyr/drivers/adc.h>
#include <zephyr/logging/log.h>
#include <stdlib.h>

LOG_MODULE_REGISTER(battery, LOG_LEVEL_INF);

static const struct adc_dt_spec adc_channel = ADC_DT_SPEC_GET_BY_IDX(DT_PATH(vbatt), 0);

#define BATTERY_SAMPLE_COUNT 15

static int compare_int16(const void *a, const void *b) {
    int16_t arg1 = *(const int16_t *)a;
    int16_t arg2 = *(const int16_t *)b;
    if (arg1 < arg2) return -1;
    if (arg1 > arg2) return 1;
    return 0;
}

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

static uint16_t g_smoothed_bat_mv = 0;

uint16_t battery_read_mv(void) {
    int16_t samples[BATTERY_SAMPLE_COUNT];
    int valid_samples = 0;

    for (int i = 0; i < BATTERY_SAMPLE_COUNT; i++) {
        int16_t sample = 0;
        struct adc_sequence sequence = {
            .buffer = &sample,
            .buffer_size = sizeof(sample),
        };

        int err = adc_sequence_init_dt(&adc_channel, &sequence);
        if (err == 0 && adc_read_dt(&adc_channel, &sequence) == 0) {
            samples[valid_samples++] = sample;
        }
        k_usleep(500); // 0.5ms delay between samples
    }

    if (valid_samples == 0) {
        LOG_ERR("All ADC samples failed!");
        return g_smoothed_bat_mv ? g_smoothed_bat_mv : 3700;
    }

    // Sort samples to select median value and reject noise spikes
    qsort(samples, valid_samples, sizeof(int16_t), compare_int16);
    int16_t median_raw = samples[valid_samples / 2];

    int32_t val_mv = median_raw;
    adc_raw_to_millivolts_dt(&adc_channel, &val_mv);

    // Multiplier ratio for nice!nano v2 battery divider (261mV measured = 3965mV actual voltage)
    uint16_t instant_mv = (uint16_t)((val_mv * 3965) / 261);

    if (g_smoothed_bat_mv == 0) {
        g_smoothed_bat_mv = instant_mv; // Initialize on first reading
    } else {
        // Exponential Moving Average (7/8 old + 1/8 new) to smooth load & display refresh fluctuations
        g_smoothed_bat_mv = (uint16_t)(((uint32_t)g_smoothed_bat_mv * 7 + instant_mv) / 8);
    }

    LOG_DBG("Battery ADC: raw=%d, val_mv=%d, instant=%u mV -> smoothed=%u mV",
            median_raw, val_mv, instant_mv, g_smoothed_bat_mv);
    return g_smoothed_bat_mv;
}

uint8_t battery_get_percentage_from_mv(uint16_t mv) {
    if (mv >= 4200) return 100;
    if (mv <= 3300) return 0;

    // Linear mapping between 3.3V (0%) and 4.2V (100%)
    return (uint8_t)(((mv - 3300) * 100) / (4200 - 3300));
}

uint8_t battery_get_percentage(void) {
    uint16_t mv = battery_read_mv();
    uint8_t pct = battery_get_percentage_from_mv(mv);
    LOG_DBG("Battery level: %u mV -> %u%%", mv, pct);
    return pct;
}
