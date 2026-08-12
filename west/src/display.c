#include "display.h"
#include "battery.h"
#include "gfxfont.h"
#include "AmazonEmber_Medium9pt7b.h"
#include "AmazonEmber_Medium12pt7b.h"
#include "Bookerly9pt7b.h"
#include "Bookerly12pt7b.h"

#include <hal/nrf_gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>
#include "display_gdey037t03.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

LOG_MODULE_REGISTER(display, LOG_LEVEL_INF);

#define EPD_CS_PIN   NRF_GPIO_PIN_MAP(0, 9)   // Pin 10 -> P0.09
#define EPD_DC_PIN   NRF_GPIO_PIN_MAP(0, 20)  // Pin 3  -> P0.20
#define EPD_RST_PIN  NRF_GPIO_PIN_MAP(0, 17)  // Pin 2  -> P0.17
#define EPD_BUSY_PIN NRF_GPIO_PIN_MAP(0, 11)  // Pin 7  -> P0.11
#define EPD_MOSI_PIN NRF_GPIO_PIN_MAP(0, 10)  // Pin 16 -> P0.10
#define EPD_SCK_PIN  NRF_GPIO_PIN_MAP(0, 29)  // Pin 20 -> P0.29
#define EPD_VCC_PIN  NRF_GPIO_PIN_MAP(1, 9)   // Pin 13 -> P1.09 (Active LOW MOSFET)
#define SD_CS_PIN    NRF_GPIO_PIN_MAP(1, 13)  // Pin 8  -> P1.13

static uint8_t epd_framebuffer[EPD_BUFFER_SIZE];
static FontType g_font_type = FONT_SERIF;
static FontSize g_font_size = SIZE_MEDIUM;
static LineSpacing g_line_spacing = SPACING_NORMAL;
static ContrastMode g_contrast_mode = CONTRAST_NORMAL;
static bool g_display_flipped = true;

static const struct device *epd_dev = DEVICE_DT_GET(DT_ALIAS(display0));

int display_init(void) {
    if (!device_is_ready(epd_dev)) {
        LOG_ERR("GDEY037T03 display device is not ready!");
        return -ENODEV;
    }
    display_clear_buffer();
    return 0;
}

void display_power_on(void) {
}

void display_power_off(void) {
    display_blanking_on(epd_dev);
}

void display_app_clear(void) {
    display_clear_buffer();
    display_update(true);
}

void display_set_flipped(bool flipped) {
    g_display_flipped = flipped;
}

bool display_is_flipped(void) {
    return g_display_flipped;
}

FontType display_get_font(void) { return g_font_type; }
void display_set_font(FontType font) { g_font_type = font; }
void display_cycle_font(void) { g_font_type = (g_font_type == FONT_SERIF) ? FONT_SANS : FONT_SERIF; }

FontSize display_get_size(void) { return g_font_size; }
void display_set_size(FontSize size) { g_font_size = size; }
void display_cycle_size(void) { g_font_size = (g_font_size == SIZE_SMALL) ? SIZE_MEDIUM : SIZE_SMALL; }

LineSpacing display_get_spacing(void) { return g_line_spacing; }
void display_set_spacing(LineSpacing spacing) { g_line_spacing = spacing; }
void display_cycle_spacing(void) { g_line_spacing = (LineSpacing)((g_line_spacing + 1) % 3); }

ContrastMode display_get_contrast(void) { return g_contrast_mode; }
void display_app_set_contrast(ContrastMode mode) { g_contrast_mode = mode; }
void display_cycle_contrast(void) { g_contrast_mode = (ContrastMode)((g_contrast_mode + 1) % 2); }

static const GFXfont* get_active_reader_font(void) {
    if (g_font_type == FONT_SANS) {
        if (g_font_size == SIZE_SMALL) return &AmazonEmber_Medium9pt7b;
        return &AmazonEmber_Medium12pt7b;
    } else { // FONT_SERIF (Bookerly)
        if (g_font_size == SIZE_SMALL) return &Bookerly9pt7b;
        return &Bookerly12pt7b;
    }
}

void display_update(bool full) {
    if (full) {
        gdey037t03_request_full_refresh(epd_dev);
    }

    struct display_buffer_descriptor desc = {
        .buf_size = EPD_BUFFER_SIZE,
        .width = EPD_PHYS_WIDTH,
        .height = EPD_PHYS_HEIGHT,
        .pitch = EPD_PHYS_WIDTH,
    };

    display_write(epd_dev, 0, 0, &desc, epd_framebuffer);
}

void display_draw_pixel(int x, int y, uint8_t color) {
    if (x < 0 || x >= EPD_WIDTH || y < 0 || y >= EPD_HEIGHT) return;

    int phys_x, phys_y;

    if (g_display_flipped) { // Rotation 3 (Flipped Landscape)
        phys_x = y;
        phys_y = (EPD_PHYS_HEIGHT - 1) - x;
    } else { // Rotation 1 (Normal Landscape)
        phys_x = (EPD_PHYS_WIDTH - 1) - y;
        phys_y = x;
    }

    if (g_contrast_mode == CONTRAST_INVERTED) {
        color = (color == 0) ? 1 : 0;
    }

    int byte_idx = (phys_x / 8) + (phys_y * (EPD_PHYS_WIDTH / 8));
    uint8_t bit_mask = 0x80 >> (phys_x % 8);

    if (color == 0) {
        epd_framebuffer[byte_idx] &= ~bit_mask;
    } else {
        epd_framebuffer[byte_idx] |= bit_mask;
    }
}

void display_fill_rect(int x, int y, int w, int h, uint8_t color) {
    for (int i = x; i < x + w; i++) {
        for (int j = y; j < y + h; j++) {
            display_draw_pixel(i, j, color);
        }
    }
}

void display_draw_rect(int x, int y, int w, int h, uint8_t color) {
    for (int i = x; i < x + w; i++) {
        display_draw_pixel(i, y, color);
        display_draw_pixel(i, y + h - 1, color);
    }
    for (int j = y; j < y + h; j++) {
        display_draw_pixel(x, j, color);
        display_draw_pixel(x + w - 1, j, color);
    }
}

void display_draw_line(int x0, int y0, int x1, int y1, uint8_t color) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;

    while (1) {
        display_draw_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static uint8_t decode_utf8_char(const char **pp) {
    const uint8_t *p = (const uint8_t *)*pp;
    if (!*p) return 0;

    uint32_t cp = 0;
    int bytes = 0;

    if ((*p & 0x80) == 0x00) {
        cp = *p;
        bytes = 1;
    } else if ((*p & 0xE0) == 0xC0) {
        cp = *p & 0x1F;
        bytes = 2;
    } else if ((*p & 0xF0) == 0xE0) {
        cp = *p & 0x0F;
        bytes = 3;
    } else if ((*p & 0xF8) == 0xF0) {
        cp = *p & 0x07;
        bytes = 4;
    } else {
        (*pp)++;
        return '?';
    }

    for (int i = 1; i < bytes; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            *pp += i;
            return '?';
        }
        cp = (cp << 6) | (p[i] & 0x3F);
    }

    *pp += bytes;

    if (cp == 0x201C || cp == 0x201D) return '"';
    if (cp == 0x2018 || cp == 0x2019) return '\'';
    if (cp == 0x2014 || cp == 0x2013) return '-';
    if (cp == 0x2026) return '.';
    if (cp == 160) return ' ';
    if (cp < 128) return (uint8_t)cp;

    return '?';
}

static int display_draw_gfx_char_raw(int x, int y, uint8_t c, const GFXfont *font, uint8_t color) {
    if (c < font->first || c > font->last) {
        c = '?';
        if (c < font->first || c > font->last) return 0;
    }

    GFXglyph *glyph = &(font->glyph[c - font->first]);
    uint8_t *bitmap = font->bitmap;

    uint32_t bo = glyph->bitmapOffset;
    uint8_t  w  = glyph->width;
    uint8_t  h  = glyph->height;
    int8_t   xo = glyph->xOffset;
    int8_t   yo = glyph->yOffset;

    uint8_t  bits = 0, bit = 0;

    for (int yy = 0; yy < h; yy++) {
        for (int xx = 0; xx < w; xx++) {
            if (!(bit++ & 7)) {
                bits = bitmap[bo++];
            }
            if (bits & 0x80) {
                display_draw_pixel(x + xo + xx, y + yo + yy, color);
            }
            bits <<= 1;
        }
    }
    return glyph->xAdvance;
}

static int display_draw_gfx_string_raw(int x, int y, const char *str, const GFXfont *font, uint8_t color) {
    int cur_x = x;
    const char *p = str;

    while (*p) {
        uint8_t c = decode_utf8_char(&p);
        if (c == 0) break;
        cur_x += display_draw_gfx_char_raw(cur_x, y, c, font, color);
    }
    return cur_x - x;
}

static int display_draw_gfx_string(int x, int y, const char *str, const GFXfont *font, uint8_t color) {
    char decoded_buf[256];
    int out_idx = 0;
    const char *p = str;

    while (*p && out_idx < sizeof(decoded_buf) - 1) {
        uint8_t c = decode_utf8_char(&p);
        if (c == 0) break;
        decoded_buf[out_idx++] = c;
    }
    decoded_buf[out_idx] = 0;

    int total_width = 0;
    for (int i = 0; i < out_idx; i++) {
        uint8_t c = decoded_buf[i];
        uint16_t glyph_idx = (c >= font->first && c <= font->last) ? (c - font->first) : ('?' - font->first);
        total_width += font->glyph[glyph_idx].xAdvance;
    }

    int max_width = EPD_WIDTH - x - 5;
    if (total_width > max_width && out_idx > 3) {
        int w = 0;
        int dot_adv = font->glyph['.' - font->first].xAdvance;
        int ellip_w = dot_adv * 3;
        int cut_idx = out_idx;

        for (int i = 0; i < out_idx; i++) {
            uint8_t c = decoded_buf[i];
            uint16_t glyph_idx = (c >= font->first && c <= font->last) ? (c - font->first) : ('?' - font->first);
            int adv = font->glyph[glyph_idx].xAdvance;

            if (w + adv + ellip_w > max_width) {
                cut_idx = i;
                break;
            }
            w += adv;
        }

        decoded_buf[cut_idx++] = '.';
        decoded_buf[cut_idx++] = '.';
        decoded_buf[cut_idx++] = '.';
        decoded_buf[cut_idx] = 0;
    }

    return display_draw_gfx_string_raw(x, y, decoded_buf, font, color);
}

void display_draw_string(int x, int y, const char *str, uint8_t color) {
    display_draw_gfx_string(x, y + 14, str, &AmazonEmber_Medium12pt7b, color);
}

void display_draw_battery(int percent) {
    int bx = EPD_WIDTH - 65;
    int by = 4;

    display_draw_rect(bx, by, 26, 13, 0);
    display_fill_rect(bx + 26, by + 4, 3, 5, 0);
    int fill_w = (20 * percent) / 100;
    if (fill_w > 0) {
        display_fill_rect(bx + 3, by + 3, fill_w, 7, 0);
    }

    // Draw percentage text on right side of battery icon, close to tip
    char pct_str[16];
    snprintf(pct_str, sizeof(pct_str), "%d%%", percent);
    display_draw_gfx_string(bx + 32, by + 12, pct_str, &AmazonEmber_Medium9pt7b, 0);
}

void display_draw_menu(const char *title, const char *options[], int count, int selected_idx) {
    display_draw_menu_ext(title, NULL, options, count, selected_idx);
}

void display_draw_menu_ext(const char *title, const char *sub_footer, const char *options[], int count, int selected_idx) {
    display_clear_buffer();

    // Compact Top Bar Header
    display_draw_gfx_string(10, 15, title, &AmazonEmber_Medium9pt7b, 0);
    display_draw_battery(battery_get_percentage());
    display_fill_rect(0, 20, EPD_WIDTH, 1, 0);

    // Menu Item Rendering with Inverted Solid Black Background Box for Active Item
    int start_y = 44;
    int item_height = (count > 6) ? 22 : 26;

    for (int i = 0; i < count; i++) {
        int y = start_y + (i * item_height);
        if (i == selected_idx) {
            // Solid Black Selection Bar
            display_fill_rect(6, y - 16, EPD_WIDTH - 12, 22, 0);
            // White Text on Black Bar
            display_draw_gfx_string(16, y, options[i], &AmazonEmber_Medium12pt7b, 1);
        } else {
            // Normal Black Text on White Background
            display_draw_gfx_string(16, y, options[i], &AmazonEmber_Medium12pt7b, 0);
        }
    }

    if (sub_footer && sub_footer[0]) {
        display_draw_gfx_string(10, EPD_HEIGHT - 6, sub_footer, &AmazonEmber_Medium9pt7b, 0);
    }

    display_update(false);
}

void display_draw_message(const char *title, const char *msg, bool is_alert) {
    display_clear_buffer();
    display_draw_gfx_string(10, 15, title, &AmazonEmber_Medium9pt7b, 0);
    display_draw_battery(battery_get_percentage());
    display_fill_rect(0, 20, EPD_WIDTH, 1, 0);
    display_draw_gfx_string(10, 44, msg, &AmazonEmber_Medium12pt7b, 0);
    display_update(false);
}

void display_draw_progress(const char *task, int percentage) {
    display_clear_buffer();
    display_draw_gfx_string(10, 15, task, &AmazonEmber_Medium9pt7b, 0);
    display_draw_battery(battery_get_percentage());
    display_fill_rect(0, 20, EPD_WIDTH, 1, 0);
    display_draw_rect(10, 44, EPD_WIDTH - 20, 16, 0);
    int fill_w = ((EPD_WIDTH - 20) * percentage) / 100;
    if (fill_w > 0) {
        display_fill_rect(10, 44, fill_w, 16, 0);
    }
    display_update(false);
}

void display_draw_lockscreen(const char *title, const char *author, const char *chapter, const char *page, int press_count) {
    display_clear_buffer();
    display_draw_gfx_string(20, 40, "SLEEPING", &AmazonEmber_Medium12pt7b, 0);
    display_draw_line(20, 50, EPD_WIDTH - 20, 50, 0);

    display_draw_gfx_string(20, 90, title, &AmazonEmber_Medium12pt7b, 0);
    display_draw_gfx_string(20, 120, author, &AmazonEmber_Medium9pt7b, 0);

    char footer[64];
    snprintf(footer, sizeof(footer), "%s | %s", chapter, page);
    display_draw_gfx_string(20, 200, footer, &AmazonEmber_Medium9pt7b, 0);

    display_update(true);
}

uint32_t display_draw_reader_page(const char *book_name, const char *text_page, uint32_t progress_pct) {
    display_clear_buffer();

    // Header Title & Battery Icon on Top Bar
    display_draw_gfx_string(10, 15, book_name, &AmazonEmber_Medium9pt7b, 0);
    display_draw_battery(battery_get_percentage());
    display_fill_rect(0, 20, EPD_WIDTH, 1, 0);

    const GFXfont *active_font = get_active_reader_font();
    int font_height = (g_font_size == SIZE_SMALL) ? 14 : 18;
    int line_height = font_height + ((g_line_spacing == SPACING_COMPACT) ? 4 : ((g_line_spacing == SPACING_RELAXED) ? 10 : 7));

    int margin_top = 41; // +3px top padding below header bar
    int margin_bottom = EPD_HEIGHT - 6;
    int margin_left = 10;
    int max_width = EPD_WIDTH - 20;

    int cur_y = margin_top;
    int max_y = margin_bottom - font_height + 4;

    const char *p = text_page;
    const char *page_start = text_page;

    while (*p && cur_y <= max_y) {
        if (*p == '\r') {
            p++;
            continue;
        }

        if (*p == '\n') {
            p++;
            while (*p == '\n' || *p == '\r') p++;
            if (cur_y > max_y) break;
            continue;
        }

        uint8_t line_buf[256] = {0};
        int line_len = 0;
        int line_width = 0;
        const char *line_start_p = p;

        while (*p && *p != '\n' && *p != '\r') {
            const char *word_start_p = p;
            uint8_t decoded_word[128] = {0};
            int word_len = 0;
            int word_width = 0;

            if (*p == ' ' || *p == '\t') {
                while (*p == ' ' || *p == '\t') p++;
                int space_adv = active_font->glyph[0].xAdvance;
                if (line_len > 0 && (line_width + space_adv <= max_width)) {
                    line_buf[line_len++] = ' ';
                    line_width += space_adv;
                }
                continue;
            }

            while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
                uint8_t c = decode_utf8_char(&p);
                if (c == 0) break;

                uint16_t glyph_idx = (c >= active_font->first && c <= active_font->last) ?
                                     (c - active_font->first) : ('?' - active_font->first);
                int adv = active_font->glyph[glyph_idx].xAdvance;

                decoded_word[word_len++] = c;
                word_width += adv;
            }

            if (line_len == 0) {
                if (word_width <= max_width) {
                    memcpy(line_buf + line_len, decoded_word, word_len);
                    line_len += word_len;
                    line_width += word_width;
                } else {
                    int chunk_w = 0;
                    int hyphen_adv = active_font->glyph['-' - active_font->first].xAdvance;
                    p = word_start_p;
                    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
                        const char *before_char = p;
                        uint8_t c = decode_utf8_char(&p);
                        uint16_t glyph_idx = (c >= active_font->first && c <= active_font->last) ?
                                             (c - active_font->first) : ('?' - active_font->first);
                        int adv = active_font->glyph[glyph_idx].xAdvance;
                        if (chunk_w + adv + hyphen_adv > max_width) {
                            p = before_char;
                            line_buf[line_len++] = '-';
                            break;
                        }
                        line_buf[line_len++] = c;
                        chunk_w += adv;
                    }
                    break;
                }
            } else {
                if (line_width + word_width <= max_width) {
                    memcpy(line_buf + line_len, decoded_word, word_len);
                    line_len += word_len;
                    line_width += word_width;
                } else {
                    p = word_start_p;
                    break;
                }
            }
        }

        line_buf[line_len] = 0;
        if (line_len > 0) {
            display_draw_gfx_string_raw(margin_left, cur_y, line_buf, active_font, 0);
            cur_y += line_height;
        } else {
            if (p == line_start_p && *p) p++;
        }
    }

    int bar_y = EPD_HEIGHT - 6;
    display_draw_rect(8, bar_y, EPD_WIDTH - 16, 4, 0);
    int fill_w = ((EPD_WIDTH - 16) * progress_pct) / 100;
    if (fill_w > 0) {
        display_fill_rect(8, bar_y, fill_w, 4, 0);
    }

    display_update(false);
    return (uint32_t)(p - page_start);
}

void display_clear_buffer(void) {
    memset(epd_framebuffer, 0xFF, EPD_BUFFER_SIZE);
}
