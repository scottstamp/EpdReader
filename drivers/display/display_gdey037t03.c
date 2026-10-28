#define DT_DRV_COMPAT gooddisplay_gdey037t03

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>

#include "display_gdey037t03.h"

LOG_MODULE_REGISTER(gdey037t03, CONFIG_DISPLAY_LOG_LEVEL);

#define EPD_PHYS_WIDTH  240
#define EPD_PHYS_HEIGHT 416
#define EPD_BUFFER_SIZE ((EPD_PHYS_WIDTH / 8) * EPD_PHYS_HEIGHT)

// With CONFIG_GDEY037T03_KEEP_POWERED the charge pump stays on between refreshes while the user is paging, and is
// switched off once the panel has been idle this long
#define EPD_IDLE_POWER_OFF_MS 2000

// How long to wait for BUSY to assert after a command before treating it as complete.
// The UC8253 can take several ms to raise BUSY (the old driver slept a fixed 10 ms);
// returning early lets the next RAM write race a refresh that is still running.
#define EPD_BUSY_ASSERT_MS 20

// Which waveform tables the controller currently holds
#define LUT_OTP     (-1)   // built-in OTP waveform (full refresh)
#define LUT_UNKNOWN (-2)

struct gdey037t03_config {
    struct spi_dt_spec spi;
    struct gpio_dt_spec dc_gpio;
    struct gpio_dt_spec reset_gpio;
    struct gpio_dt_spec busy_gpio;
    struct gpio_dt_spec vcc_gpio;
};

#ifndef CONFIG_GDEY037T03_DEFAULT_REFRESH_PROFILE
#define CONFIG_GDEY037T03_DEFAULT_REFRESH_PROFILE 0
#endif
#ifndef CONFIG_GDEY037T03_CUSTOM_PULSE_INTERVAL
#define CONFIG_GDEY037T03_CUSTOM_PULSE_INTERVAL 10
#endif
#ifndef CONFIG_GDEY037T03_CUSTOM_PULSE_COUNT
#define CONFIG_GDEY037T03_CUSTOM_PULSE_COUNT 1
#endif
#ifndef CONFIG_GDEY037T03_CUSTOM_FRAME_RATE
#define CONFIG_GDEY037T03_CUSTOM_FRAME_RATE 0
#endif

struct gdey037t03_data {
    const struct device *dev;
    enum display_orientation orientation;
    bool force_full_refresh;
    bool draw_from_white; // next full refresh clears to white, then draws with the partial waveform
    bool powered;      // charge pump on (0x04 issued, no 0x02 since)
    bool vcc_off;      // panel supply cut; controller state and RAM are lost
    int lut_loaded;    // LUT_OTP, LUT_UNKNOWN or the partial waveform signature
    bool pll_set;      // PLL (0x30) changed from its reset default since the last soft reset
    struct gdey037t03_refresh_config refresh_cfg;
    struct k_mutex lock;
    struct k_sem busy_sem;
    struct gpio_callback busy_cb;
    struct k_work_delayable power_off_work;
};

static void epd_write_cmd(const struct gdey037t03_config *config, uint8_t cmd) {
    gpio_pin_set_dt(&config->dc_gpio, 0); // DC = Command (0)
    struct spi_buf buf = {
        .buf = &cmd,
        .len = 1,
    };
    struct spi_buf_set tx = {
        .buffers = &buf,
        .count = 1,
    };
    spi_write_dt(&config->spi, &tx);
}

static void epd_write_data(const struct gdey037t03_config *config, uint8_t data) {
    gpio_pin_set_dt(&config->dc_gpio, 1); // DC = Data (1)
    struct spi_buf buf = {
        .buf = &data,
        .len = 1,
    };
    struct spi_buf_set tx = {
        .buffers = &buf,
        .count = 1,
    };
    spi_write_dt(&config->spi, &tx);
}

static void epd_write_data_bytes(const struct gdey037t03_config *config, const uint8_t *data, size_t len) {
    gpio_pin_set_dt(&config->dc_gpio, 1); // DC = Data (1)
    struct spi_buf buf = {
        .buf = (void *)data,
        .len = len,
    };
    struct spi_buf_set tx = {
        .buffers = &buf,
        .count = 1,
    };
    spi_write_dt(&config->spi, &tx);
}

static void epd_busy_released(const struct device *port, struct gpio_callback *cb, uint32_t pins) {
    struct gdey037t03_data *data = CONTAINER_OF(cb, struct gdey037t03_data, busy_cb);
    k_sem_give(&data->busy_sem);
}

// Blocks until BUSY (active low on this panel) is released. busy_sem must have been
// reset before the operation started, so a release edge that already fired is not missed.
// The edge only wakes the wait; the pin level decides, so a glitch on BUSY mid-refresh
// doesn't end it early (power-off would then cut the refresh short and leave part of the
// image faded).
static void epd_wait_idle(const struct device *dev, const char *stage_name, int timeout_ms) {
    const struct gdey037t03_config *config = dev->config;
    struct gdey037t03_data *data = dev->data;
    int64_t deadline = k_uptime_get() + timeout_ms;

    for (;;) {
        while (gpio_pin_get_dt(&config->busy_gpio) == 1) {
            int64_t left = deadline - k_uptime_get();
            if (left <= 0) {
                LOG_WRN("EPD BUSY timeout during %s after %d ms", stage_name, timeout_ms);
                return;
            }
            k_sem_take(&data->busy_sem, K_MSEC(left));
        }
        // A real release stays released
        k_msleep(1);
        if (gpio_pin_get_dt(&config->busy_gpio) == 0) {
            return;
        }
        LOG_WRN("EPD BUSY glitch during %s", stage_name);
    }
}

// Sends cmd and blocks until the controller has finished it
static void epd_cmd_wait(const struct device *dev, uint8_t cmd, const char *stage_name, int timeout_ms) {
    const struct gdey037t03_config *config = dev->config;
    struct gdey037t03_data *data = dev->data;

    k_sem_reset(&data->busy_sem);
    epd_write_cmd(config, cmd);

    uint32_t start = k_uptime_get_32();
    while (k_uptime_get_32() - start < EPD_BUSY_ASSERT_MS) {
        if (gpio_pin_get_dt(&config->busy_gpio) == 1) {
            epd_wait_idle(dev, stage_name, timeout_ms);
            return;
        }
        k_sleep(K_USEC(200));
    }
    LOG_WRN("EPD BUSY never asserted during %s", stage_name);
}

static void epd_set_partial_ram_area(const struct gdey037t03_config *config, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    uint16_t xe = (x + w - 1) | 0x0007; // byte boundary inclusive
    uint16_t ye = y + h - 1;
    x &= 0xFFF8; // byte boundary
    epd_write_cmd(config, 0x90); // partial window
    epd_write_data(config, x);
    epd_write_data(config, xe);
    epd_write_data(config, y / 256);
    epd_write_data(config, y % 256);
    epd_write_data(config, ye / 256);
    epd_write_data(config, ye % 256);
    epd_write_data(config, 0x01);
}

static void epd_write_ram(const struct gdey037t03_config *config, uint8_t ram_cmd, const void *buf, size_t len) {
    epd_write_cmd(config, 0x91); // partial in
    epd_set_partial_ram_area(config, 0, 0, EPD_PHYS_WIDTH, EPD_PHYS_HEIGHT);
    epd_write_cmd(config, ram_cmd);
    epd_write_data_bytes(config, buf, len);
    epd_write_cmd(config, 0x92); // partial out
}

static void epd_fill_ram(const struct gdey037t03_config *config, uint8_t ram_cmd, uint8_t value, size_t len) {
    uint8_t chunk[256];
    memset(chunk, value, sizeof(chunk));
    epd_write_cmd(config, 0x91); // partial in
    epd_set_partial_ram_area(config, 0, 0, EPD_PHYS_WIDTH, EPD_PHYS_HEIGHT);
    epd_write_cmd(config, ram_cmd);
    for (size_t off = 0; off < len; off += sizeof(chunk)) {
        epd_write_data_bytes(config, chunk, MIN(sizeof(chunk), len - off));
    }
    epd_write_cmd(config, 0x92); // partial out
}

// Soft reset into OTP waveform mode. Resets registers, so the charge pump is off afterwards.
static void epd_init_display(const struct device *dev) {
    const struct gdey037t03_config *config = dev->config;
    struct gdey037t03_data *data = dev->data;

    epd_write_cmd(config, 0x00); // PANEL SETTING
    epd_write_data(config, 0x1E); // soft reset
    epd_write_data(config, 0x0D);
    k_msleep(1);

    epd_write_cmd(config, 0x00); // PANEL SETTING
    epd_write_data(config, 0x1F); // BWOTP mode
    epd_write_data(config, 0x0D);

    data->powered = false;
    data->lut_loaded = LUT_OTP;
    data->pll_set = false;
}

static void epd_power_on(const struct device *dev) {
    struct gdey037t03_data *data = dev->data;
    if (!data->powered) {
        epd_cmd_wait(dev, 0x04, "PowerOn", 500);
        data->powered = true;
    }
}

static void epd_power_off(const struct device *dev) {
    struct gdey037t03_data *data = dev->data;
    if (data->powered) {
        epd_cmd_wait(dev, 0x02, "PowerOff", 200);
        data->powered = false;
    }
}

static void epd_power_off_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct gdey037t03_data *data = CONTAINER_OF(dwork, struct gdey037t03_data, power_off_work);

    k_mutex_lock(&data->lock, K_FOREVER);
    epd_power_off(data->dev);
    k_mutex_unlock(&data->lock);
}

// Supply on + hardware reset + OTP init
static void epd_hw_start(const struct device *dev) {
    const struct gdey037t03_config *config = dev->config;
    struct gdey037t03_data *data = dev->data;

    if (config->vcc_gpio.port) {
        gpio_pin_set_dt(&config->vcc_gpio, 1); // Power ON (Active LOW -> Physical 0)
        k_msleep(50);
    }
    if (config->spi.config.cs.gpio.port) {
        gpio_pin_configure_dt(&config->spi.config.cs.gpio, GPIO_OUTPUT_INACTIVE);
    }

    // Hardware Reset Sequence: Assert Reset (Physical 0) for 20ms, then Release Reset (Physical 1) for 50ms
    k_sem_reset(&data->busy_sem);
    gpio_pin_set_dt(&config->reset_gpio, 1);
    k_msleep(20);
    gpio_pin_set_dt(&config->reset_gpio, 0);
    k_msleep(50);

    epd_wait_idle(dev, "InitReset", 1000);
    epd_init_display(dev);

    data->vcc_off = false;
}

static int gdey037t03_init(const struct device *dev) {
    const struct gdey037t03_config *config = dev->config;
    struct gdey037t03_data *data = dev->data;

    if (!spi_is_ready_dt(&config->spi)) {
        LOG_ERR("SPI device not ready");
        return -ENODEV;
    }

    data->dev = dev;
    data->lut_loaded = LUT_UNKNOWN;
    k_mutex_init(&data->lock);
    k_sem_init(&data->busy_sem, 0, 1);
    k_work_init_delayable(&data->power_off_work, epd_power_off_work_handler);

    if (config->vcc_gpio.port) {
        gpio_pin_configure_dt(&config->vcc_gpio, GPIO_OUTPUT_INACTIVE);
    }
    gpio_pin_configure_dt(&config->dc_gpio, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&config->reset_gpio, GPIO_OUTPUT_INACTIVE); // Released (Physical 1)
    gpio_pin_configure_dt(&config->busy_gpio, GPIO_INPUT);

    gpio_init_callback(&data->busy_cb, epd_busy_released, BIT(config->busy_gpio.pin));
    gpio_add_callback(config->busy_gpio.port, &data->busy_cb);
    gpio_pin_interrupt_configure_dt(&config->busy_gpio, GPIO_INT_EDGE_TO_INACTIVE);

    epd_hw_start(dev);

    LOG_INF("GDEY037T03 (UC8253) e-Paper driver initialized on Hardware SPIM3");
    return 0;
}

void gdey037t03_request_full_refresh(const struct device *dev) {
    struct gdey037t03_data *data = dev->data;
    data->force_full_refresh = true;
}

void gdey037t03_request_draw_from_white(const struct device *dev) {
    struct gdey037t03_data *data = dev->data;
    data->draw_from_white = true;
}

// UC8253 PLL (0x30) FRS[4:0] per datasheet: 5 Hz steps from 5 Hz (0) to 120 Hz (23),
// then 10 Hz steps up to 200 Hz (31). Returns the nearest code.
static uint8_t epd_frs_from_hz(uint8_t hz) {
    if (hz <= 120) {
        return (hz < 5) ? 0 : (hz + 2) / 5 - 1;
    }
    return MIN(23 + (hz - 120 + 5) / 10, 31);
}

static uint8_t epd_hz_from_frs(uint8_t frs) {
    return (frs <= 23) ? (frs + 1) * 5 : 120 + (frs - 23) * 10;
}

int gdey037t03_set_refresh_config(const struct device *dev, const struct gdey037t03_refresh_config *cfg) {
    if (!dev || !cfg) {
        return -EINVAL;
    }
    struct gdey037t03_data *data = dev->data;
    data->refresh_cfg = *cfg;
    if (data->refresh_cfg.pulse_interval < 1) data->refresh_cfg.pulse_interval = 1;
    if (data->refresh_cfg.pulse_interval > 15) data->refresh_cfg.pulse_interval = 15;
    if (data->refresh_cfg.pulse_count < 1) data->refresh_cfg.pulse_count = 1;
    if (data->refresh_cfg.pulse_count > 5) data->refresh_cfg.pulse_count = 5;
    if (data->refresh_cfg.frame_rate != 0) {
        data->refresh_cfg.frame_rate = epd_hz_from_frs(epd_frs_from_hz(data->refresh_cfg.frame_rate));
    }
    LOG_INF("EPD Refresh config: profile=%d, interval=%u, count=%u, frame rate=%u Hz (0=default)",
            data->refresh_cfg.profile, data->refresh_cfg.pulse_interval, data->refresh_cfg.pulse_count,
            data->refresh_cfg.frame_rate);
    return 0;
}

int gdey037t03_get_refresh_config(const struct device *dev, struct gdey037t03_refresh_config *cfg) {
    if (!dev || !cfg) {
        return -EINVAL;
    }
    const struct gdey037t03_data *data = dev->data;
    *cfg = data->refresh_cfg;
    return 0;
}

// Normal's frame rate: tuned on the GDEY037T03 (interval 2, 3 pulses at 80 Hz erases cleanly)
#define NORMAL_FRAME_RATE_HZ 80

// *hz = 0 leaves the PLL at the controller default
static void partial_waveform_params(const struct gdey037t03_data *data, uint8_t *t, uint8_t *rp, uint8_t *hz) {
    *hz = 0;
    switch (data->refresh_cfg.profile) {
    case GDEY037T03_PROFILE_FAST:
        *t = 2;
        *rp = 2;
        break;
    case GDEY037T03_PROFILE_TURBO:
        *t = 1;
        *rp = 1;
        break;
    case GDEY037T03_PROFILE_CUSTOM:
        *t = CLAMP(data->refresh_cfg.pulse_interval, 1, 15);
        *rp = CLAMP(data->refresh_cfg.pulse_count, 1, 5);
        *hz = data->refresh_cfg.frame_rate;
        break;
    default: // GDEY037T03_PROFILE_NORMAL
        *t = 2;
        *rp = 3;
        *hz = NORMAL_FRAME_RATE_HZ;
        break;
    }
}

static void epd_load_partial_lut(const struct gdey037t03_config *config, uint8_t t, uint8_t rp) {
    uint8_t lut_vcom[56] = {0};
    uint8_t lut_ww[42] = {0};
    uint8_t lut_bw[56] = {0};
    uint8_t lut_wb[42] = {0};
    uint8_t lut_bb[42] = {0};

    uint8_t v_white = 0x80 | (t & 0x0F); // Drive white (VSL polarity)
    uint8_t v_black = 0x40 | (t & 0x0F); // Drive black (VSH polarity)

    // True Differential Partial Refresh:
    // - lut_ww (0x21) = 0: Unchanged white pixels receive 0V (no invert, no background flash)
    // - lut_bb (0x24) = 0: Unchanged black pixels receive 0V (no flicker)
    // - lut_vcom (0x20)= 0: VCOM stays neutral
    // - lut_bw (0x22) drives erased black pixels back to white
    // - lut_wb (0x23) drives newly rendered pixels to black

    // LUT 0x22 (BW): Transition Black -> White (clearing previous text)
    lut_bw[0] = 0x01;
    lut_bw[1] = v_white;
    lut_bw[2] = v_white;
    lut_bw[3] = v_white;
    lut_bw[4] = v_white;
    lut_bw[5] = (rp < 5) ? (rp + 1) : rp; // Extra pulse to overcome VSL rail asymmetry and fully erase black
    lut_bw[6] = 0x01;

    // LUT 0x23 (WB): Transition White -> Black (drawing new text)
    lut_wb[0] = 0x01;
    lut_wb[1] = v_black;
    lut_wb[2] = v_black;
    lut_wb[3] = v_black;
    lut_wb[4] = v_black;
    lut_wb[5] = rp;
    lut_wb[6] = 0x01;

    // Panel setting: 0xFF (enable register LUTs)
    epd_write_cmd(config, 0x00);
    epd_write_data(config, 0xFF);

    // VCOM and Data Interval Setting (CDI): 0xD7
    epd_write_cmd(config, 0x50);
    epd_write_data(config, 0xD7);

    // Write full register LUT banks
    epd_write_cmd(config, 0x20);
    epd_write_data_bytes(config, lut_vcom, sizeof(lut_vcom));

    epd_write_cmd(config, 0x21);
    epd_write_data_bytes(config, lut_ww, sizeof(lut_ww));

    epd_write_cmd(config, 0x22);
    epd_write_data_bytes(config, lut_bw, sizeof(lut_bw));

    epd_write_cmd(config, 0x23);
    epd_write_data_bytes(config, lut_wb, sizeof(lut_wb));

    epd_write_cmd(config, 0x24);
    epd_write_data_bytes(config, lut_bb, sizeof(lut_bb));
}

static int gdey037t03_write(const struct device *dev, const uint16_t x, const uint16_t y,
                            const struct display_buffer_descriptor *desc, const void *buf) {
    const struct gdey037t03_config *config = dev->config;
    struct gdey037t03_data *data = dev->data;

    if (buf == NULL || desc->buf_size == 0) {
        return -EINVAL;
    }

    k_mutex_lock(&data->lock, K_FOREVER);
    k_work_cancel_delayable(&data->power_off_work);

    uint32_t t_start = k_uptime_get_32();

    if (data->vcc_off) {
        epd_hw_start(dev);
        data->force_full_refresh = true; // panel RAM was lost with the supply
    }

    bool full = data->force_full_refresh;
    data->force_full_refresh = false;
    // Full refresh to white, then draw the frame with the partial waveform
    bool from_white = full && data->draw_from_white;
    data->draw_from_white = false;

    uint32_t t_tx, t_pwr, t_refresh;

    if (full) {
        // Back to the OTP waveform (a no-op reset if already there, cheap next to a full refresh)
        epd_init_display(dev);

        // Stream target frame to both RAMs so the next partial refresh diffs against it.
        // Dithered images (covers) fade toward white the moment the OTP waveform ends, so
        // those only get a white frame here and are drawn white->black by the partial
        // waveform below, which holds them.
        if (from_white) {
            epd_fill_ram(config, 0x13, 0xFF, desc->buf_size);
            epd_fill_ram(config, 0x10, 0xFF, desc->buf_size);
        } else {
            epd_write_ram(config, 0x13, buf, desc->buf_size);
            epd_write_ram(config, 0x10, buf, desc->buf_size);
        }
        t_tx = k_uptime_get_32();

        // Pure OTP Full Blanking Refresh
        epd_write_cmd(config, 0x50);
        epd_write_data(config, 0x97);

        epd_power_on(dev);
        t_pwr = k_uptime_get_32();
        // OTP full refresh slows down in the cold; a timeout here powers off mid-refresh
        epd_cmd_wait(dev, 0x12, "UpdateFull", 10000);
        t_refresh = k_uptime_get_32();

        if (!IS_ENABLED(CONFIG_GDEY037T03_KEEP_POWERED)) {
            k_msleep(200);
            epd_power_off(dev);
            epd_init_display(dev);
        }

        if (from_white) {
            // Old frame for the partial draw below
            epd_fill_ram(config, 0x10, 0xFF, desc->buf_size);
            LOG_INF("EPD full refresh to white: refresh %u ms", t_refresh - t_pwr);
        }
    }

    if (!full || from_white) {
        uint8_t t, rp, hz;
        partial_waveform_params(data, &t, &rp, &hz);
        int signature = (hz << 8) | (t << 4) | rp;
        if (hz == 0 && data->pll_set) {
            // Only reachable with KEEP_POWERED: a soft reset is the one way back to the default
            // frame rate, whose value the datasheet doesn't pin down (06h in one table, 09h in another)
            epd_power_off(dev);
            epd_init_display(dev);
        }

        // New frame into RAM 0x13; RAM 0x10 still holds the frame on screen
        epd_write_ram(config, 0x13, buf, desc->buf_size);

        if (data->lut_loaded != signature) {
            epd_load_partial_lut(config, t, rp);
            if (hz != 0) {
                // The soft reset after every refresh restores the default, so this is
                // rewritten each time along with the LUT
                epd_write_cmd(config, 0x30);
                epd_write_data(config, epd_frs_from_hz(hz));
                data->pll_set = true;
            }
            data->lut_loaded = signature;
        }
        t_tx = k_uptime_get_32();

        epd_power_on(dev);
        t_pwr = k_uptime_get_32();
        epd_cmd_wait(dev, 0x12, "UpdatePart", 2000);
        t_refresh = k_uptime_get_32();

        if (!IS_ENABLED(CONFIG_GDEY037T03_KEEP_POWERED)) {
            // Known-good sequence from the original driver: without the power-off and
            // soft reset here, old pixels stop being erased and pages pile up
            k_msleep(60);
            epd_power_off(dev);
            epd_init_display(dev); // also forces the partial waveform to be reloaded next time
        }

        // The new frame becomes the "previous" frame for the next differential refresh
        epd_write_ram(config, 0x10, buf, desc->buf_size);
    }

    LOG_INF("EPD %s refresh: tx %u ms, power-on %u ms, refresh %u ms, total %u ms",
            from_white ? "full+draw" : full ? "full" : "partial", t_tx - t_start, t_pwr - t_tx, t_refresh - t_pwr,
            k_uptime_get_32() - t_start);

    k_work_schedule(&data->power_off_work, K_MSEC(EPD_IDLE_POWER_OFF_MS));
    k_mutex_unlock(&data->lock);
    return 0;
}

static int gdey037t03_read(const struct device *dev, const uint16_t x, const uint16_t y,
                           const struct display_buffer_descriptor *desc, void *buf) {
    return -ENOTSUP;
}

static void gdey037t03_get_capabilities(const struct device *dev, struct display_capabilities *caps) {
    struct gdey037t03_data *data = dev->data;

    memset(caps, 0, sizeof(struct display_capabilities));

    if (data->orientation == DISPLAY_ORIENTATION_ROTATED_90 ||
        data->orientation == DISPLAY_ORIENTATION_ROTATED_270) {
        caps->x_resolution = EPD_PHYS_HEIGHT; // 416
        caps->y_resolution = EPD_PHYS_WIDTH;  // 240
    } else {
        caps->x_resolution = EPD_PHYS_WIDTH;  // 240
        caps->y_resolution = EPD_PHYS_HEIGHT; // 416
    }

    caps->supported_pixel_formats = PIXEL_FORMAT_MONO10; // Black and white 1-bit
    caps->current_pixel_format = PIXEL_FORMAT_MONO10;
    caps->current_orientation = data->orientation;
    caps->screen_info = SCREEN_INFO_MONO_MSB_FIRST;
}

static int gdey037t03_set_pixel_format(const struct device *dev, const enum display_pixel_format format) {
    if (format == PIXEL_FORMAT_MONO10) {
        return 0;
    }
    return -ENOTSUP;
}

static int gdey037t03_set_orientation(const struct device *dev, const enum display_orientation orientation) {
    struct gdey037t03_data *data = dev->data;
    data->orientation = orientation;
    return 0;
}

static int gdey037t03_blanking_off(const struct device *dev) {
    gdey037t03_request_full_refresh(dev);
    return 0;
}

// Used before System OFF: the image stays on the panel without power
static int gdey037t03_blanking_on(const struct device *dev) {
    const struct gdey037t03_config *config = dev->config;
    struct gdey037t03_data *data = dev->data;

    k_mutex_lock(&data->lock, K_FOREVER);
    k_work_cancel_delayable(&data->power_off_work);
    epd_power_off(dev);
    // Never cut the supply while the controller is still driving the panel
    epd_wait_idle(dev, "Sleep", 1000);

    if (config->vcc_gpio.port) {
        gpio_pin_set_dt(&config->vcc_gpio, 0);
        // Don't back-power the unpowered controller through its inputs: drive them low,
        // and float chip-select, whose idle level is high
        gpio_pin_set_dt(&config->dc_gpio, 0);
        gpio_pin_set_dt(&config->reset_gpio, 1);
        if (config->spi.config.cs.gpio.port) {
            gpio_pin_configure_dt(&config->spi.config.cs.gpio, GPIO_DISCONNECTED);
        }
        data->vcc_off = true;
    }
    k_mutex_unlock(&data->lock);
    return 0;
}

static const struct display_driver_api gdey037t03_driver_api = {
    .blanking_on = gdey037t03_blanking_on,
    .blanking_off = gdey037t03_blanking_off,
    .write = gdey037t03_write,
    .read = gdey037t03_read,
    .get_capabilities = gdey037t03_get_capabilities,
    .set_pixel_format = gdey037t03_set_pixel_format,
    .set_orientation = gdey037t03_set_orientation,
};

#define GDEY037T03_INIT(inst)                                                       \
    static struct gdey037t03_data gdey037t03_data_##inst = {                       \
        .orientation = DISPLAY_ORIENTATION_ROTATED_270,                            \
        .force_full_refresh = true,                                                 \
        .refresh_cfg = {                                                            \
            .profile = (enum gdey037t03_refresh_profile)CONFIG_GDEY037T03_DEFAULT_REFRESH_PROFILE, \
            .pulse_interval = CONFIG_GDEY037T03_CUSTOM_PULSE_INTERVAL,              \
            .pulse_count = CONFIG_GDEY037T03_CUSTOM_PULSE_COUNT,                    \
            .frame_rate = CONFIG_GDEY037T03_CUSTOM_FRAME_RATE,                      \
        },                                                                          \
    };                                                                              \
                                                                                    \
    static const struct gdey037t03_config gdey037t03_config_##inst = {             \
        .spi = SPI_DT_SPEC_INST_GET(inst, SPI_OP_MODE_MASTER | SPI_WORD_SET(8)),    \
        .dc_gpio = GPIO_DT_SPEC_INST_GET(inst, dc_gpios),                          \
        .reset_gpio = GPIO_DT_SPEC_INST_GET(inst, reset_gpios),                    \
        .busy_gpio = GPIO_DT_SPEC_INST_GET(inst, busy_gpios),                      \
        .vcc_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, vcc_gpios, {0}),                 \
    };                                                                              \
                                                                                    \
    DEVICE_DT_INST_DEFINE(inst,                                                     \
                          gdey037t03_init,                                          \
                          NULL,                                                     \
                          &gdey037t03_data_##inst,                                  \
                          &gdey037t03_config_##inst,                                \
                          POST_KERNEL,                                              \
                          CONFIG_DISPLAY_INIT_PRIORITY,                             \
                          &gdey037t03_driver_api);

DT_INST_FOREACH_STATUS_OKAY(GDEY037T03_INIT)
