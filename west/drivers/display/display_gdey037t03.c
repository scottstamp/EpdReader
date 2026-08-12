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

struct gdey037t03_config {
    struct gpio_dt_spec dc_gpio;
    struct gpio_dt_spec reset_gpio;
    struct gpio_dt_spec busy_gpio;
    struct gpio_dt_spec vcc_gpio;
    struct gpio_dt_spec cs_gpio;
    struct gpio_dt_spec mosi_gpio;
    struct gpio_dt_spec sck_gpio;
};

struct gdey037t03_data {
    enum display_orientation orientation;
    bool force_full_refresh;
};

static inline void spi_delay(void) {
    __NOP();
}

static void epd_spi_write_byte(const struct gdey037t03_config *config, uint8_t data) {
    for (int i = 7; i >= 0; i--) {
        if (data & (1 << i)) {
            gpio_pin_set_dt(&config->mosi_gpio, 1);
        } else {
            gpio_pin_set_dt(&config->mosi_gpio, 0);
        }
        gpio_pin_set_dt(&config->sck_gpio, 1);
        spi_delay();
        gpio_pin_set_dt(&config->sck_gpio, 0);
        spi_delay();
    }
}

static void epd_write_cmd(const struct gdey037t03_config *config, uint8_t cmd) {
    gpio_pin_set_dt(&config->dc_gpio, 0); // DC = Command (0)
    gpio_pin_set_dt(&config->cs_gpio, 1); // CS = Select Chip (Logical 1 -> Physical LOW for ACTIVE_LOW)
    epd_spi_write_byte(config, cmd);
    gpio_pin_set_dt(&config->cs_gpio, 0); // CS = Deselect Chip (Logical 0 -> Physical HIGH for ACTIVE_LOW)
}

static void epd_write_data(const struct gdey037t03_config *config, uint8_t data) {
    gpio_pin_set_dt(&config->dc_gpio, 1); // DC = Data (1)
    gpio_pin_set_dt(&config->cs_gpio, 1); // CS = Select Chip
    epd_spi_write_byte(config, data);
    gpio_pin_set_dt(&config->cs_gpio, 0); // CS = Deselect Chip
}

static void epd_write_data_bytes(const struct gdey037t03_config *config, const uint8_t *data, size_t len) {
    gpio_pin_set_dt(&config->dc_gpio, 1);
    gpio_pin_set_dt(&config->cs_gpio, 1);
    for (size_t i = 0; i < len; i++) {
        epd_spi_write_byte(config, data[i]);
    }
    gpio_pin_set_dt(&config->cs_gpio, 0);
}

static void epd_wait_busy(const struct gdey037t03_config *config, const char *stage_name, int timeout_ms) {
    k_msleep(10); // Allow 10ms for UC8253 controller to assert BUSY pin
    uint32_t start = k_uptime_get_32();
    // GDEY037T03 BUSY pin is ACTIVE LOW (0=BUSY). With GPIO_ACTIVE_LOW, gpio_pin_get_dt returns 1 when BUSY.
    while (gpio_pin_get_dt(&config->busy_gpio) == 1) {
        k_msleep(2);
        if (k_uptime_get_32() - start >= timeout_ms) {
            LOG_WRN("EPD WaitBusy timeout during %s after %u ms", stage_name, k_uptime_get_32() - start);
            break;
        }
    }
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

static void epd_init_display(const struct gdey037t03_config *config) {
    epd_write_cmd(config, 0x00); // PANEL SETTING
    epd_write_data(config, 0x1E); // soft reset
    epd_write_data(config, 0x0D);
    k_msleep(1);

    epd_write_cmd(config, 0x00); // PANEL SETTING
    epd_write_data(config, 0x1F); // BWOTP mode
    epd_write_data(config, 0x0D);
}

static void epd_power_on_chip(const struct gdey037t03_config *config) {
    epd_write_cmd(config, 0x04);
    epd_wait_busy(config, "PowerOnChip", 500);
}

static void epd_power_off_chip(const struct gdey037t03_config *config) {
    epd_write_cmd(config, 0x02);
    epd_wait_busy(config, "PowerOffChip", 200);
}

static int gdey037t03_init(const struct device *dev) {
    const struct gdey037t03_config *config = dev->config;

    if (config->vcc_gpio.port) {
        gpio_pin_configure_dt(&config->vcc_gpio, GPIO_OUTPUT_ACTIVE); // Power ON (Active LOW -> Physical 0)
        k_msleep(50);
    }

    gpio_pin_configure_dt(&config->cs_gpio, GPIO_OUTPUT_INACTIVE); // Deselected (Physical 1)
    gpio_pin_configure_dt(&config->dc_gpio, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&config->reset_gpio, GPIO_OUTPUT_INACTIVE); // Released (Physical 1)
    gpio_pin_configure_dt(&config->mosi_gpio, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&config->sck_gpio, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&config->busy_gpio, GPIO_INPUT);

    gpio_pin_set_dt(&config->cs_gpio, 0); // Deselected
    gpio_pin_set_dt(&config->sck_gpio, 0); // Clock Low

    // Hardware Reset Sequence: Assert Reset (Physical 0) for 20ms, then Release Reset (Physical 1) for 50ms
    gpio_pin_set_dt(&config->reset_gpio, 1);
    k_msleep(20);
    gpio_pin_set_dt(&config->reset_gpio, 0);
    k_msleep(50);

    epd_wait_busy(config, "InitReset", 1000);
    epd_init_display(config);

    LOG_INF("GDEY037T03 (UC8253) e-Paper driver initialized cleanly");
    return 0;
}

void gdey037t03_request_full_refresh(const struct device *dev) {
    struct gdey037t03_data *data = dev->data;
    data->force_full_refresh = true;
}

static int gdey037t03_write(const struct device *dev, const uint16_t x, const uint16_t y,
                            const struct display_buffer_descriptor *desc, const void *buf) {
    const struct gdey037t03_config *config = dev->config;
    struct gdey037t03_data *data = dev->data;

    if (buf == NULL || desc->buf_size == 0) {
        return -EINVAL;
    }

    bool full = data->force_full_refresh;
    data->force_full_refresh = false;

    if (config->vcc_gpio.port) {
        gpio_pin_set_dt(&config->vcc_gpio, 1);
        k_msleep(10);
    }

    if (full) {
        // Stream target frame to RAM 0x13
        epd_write_cmd(config, 0x91);
        epd_set_partial_ram_area(config, 0, 0, EPD_PHYS_WIDTH, EPD_PHYS_HEIGHT);
        epd_write_cmd(config, 0x13);
        epd_write_data_bytes(config, buf, desc->buf_size);
        epd_write_cmd(config, 0x92);

        // Stream target frame to RAM 0x10
        epd_write_cmd(config, 0x91);
        epd_set_partial_ram_area(config, 0, 0, EPD_PHYS_WIDTH, EPD_PHYS_HEIGHT);
        epd_write_cmd(config, 0x10);
        epd_write_data_bytes(config, buf, desc->buf_size);
        epd_write_cmd(config, 0x92);

        // Pure OTP Full Blanking Refresh
        epd_write_cmd(config, 0x50);
        epd_write_data(config, 0x97);

        epd_power_on_chip(config);
        epd_write_cmd(config, 0x12);
        epd_wait_busy(config, "UpdateFull", 3500);
        k_msleep(200);
        epd_power_off_chip(config);

        epd_init_display(config);
    } else {
        // Stream frame to current RAM (0x13)
        epd_write_cmd(config, 0x91);
        epd_set_partial_ram_area(config, 0, 0, EPD_PHYS_WIDTH, EPD_PHYS_HEIGHT);
        epd_write_cmd(config, 0x13);
        epd_write_data_bytes(config, buf, desc->buf_size);
        epd_write_cmd(config, 0x92);

        // Fast 0.3s Partial Refresh with 0x6E, 0xD7
        epd_write_cmd(config, 0xE0);
        epd_write_data(config, 0x02);
        epd_write_cmd(config, 0xE5);
        epd_write_data(config, 0x6E);
        epd_write_cmd(config, 0x50);
        epd_write_data(config, 0xD7);

        epd_power_on_chip(config);
        epd_write_cmd(config, 0x12);
        epd_wait_busy(config, "UpdatePart", 1000);
        k_msleep(150);
        epd_power_off_chip(config);

        epd_init_display(config);

        // Stream frame to previous RAM (0x10)
        epd_write_cmd(config, 0x91);
        epd_set_partial_ram_area(config, 0, 0, EPD_PHYS_WIDTH, EPD_PHYS_HEIGHT);
        epd_write_cmd(config, 0x10);
        epd_write_data_bytes(config, buf, desc->buf_size);
        epd_write_cmd(config, 0x92);
    }

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

static int gdey037t03_blanking_on(const struct device *dev) {
    const struct gdey037t03_config *config = dev->config;
    epd_power_off_chip(config);
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
    };                                                                              \
                                                                                    \
    static const struct gdey037t03_config gdey037t03_config_##inst = {             \
        .dc_gpio = GPIO_DT_SPEC_INST_GET(inst, dc_gpios),                          \
        .reset_gpio = GPIO_DT_SPEC_INST_GET(inst, reset_gpios),                    \
        .busy_gpio = GPIO_DT_SPEC_INST_GET(inst, busy_gpios),                      \
        .cs_gpio = GPIO_DT_SPEC_INST_GET(inst, cs_gpios),                          \
        .mosi_gpio = GPIO_DT_SPEC_INST_GET(inst, mosi_gpios),                      \
        .sck_gpio = GPIO_DT_SPEC_INST_GET(inst, sck_gpios),                        \
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
