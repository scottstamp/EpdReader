#include "display.h"
#include "battery.h"
#include "storage.h"
#include "epub.h"
#include "font_extended.h"
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
        return ' ';
    }

    for (int i = 1; i < bytes; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            *pp += i;
            return ' ';
        }
        cp = (cp << 6) | (p[i] & 0x3F);
    }

    *pp += bytes;

    // 1. Direct ASCII
    if (cp >= 32 && cp <= 126) return (uint8_t)cp;

    // 2. Spaces & Control Breaks
    if (cp == 160 || cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0x202F || cp == 0xFEFF) return ' ';

    // 3. Quotes & Punctuation
    if (cp == 0x201C || cp == 0x201D || cp == 0x201E || cp == 0x201F || cp == 0x00AB || cp == 0x00BB) return '"';
    if (cp == 0x2018 || cp == 0x2019 || cp == 0x201A || cp == 0x201B) return '\'';
    if (cp == 0x2013 || cp == 0x2014 || cp == 0x2212 || cp == 0x2010 || cp == 0x2011 || cp == 0x2012 || cp == 0x2015) return '-';
    if (cp == 0x2026) return '.';
    if (cp == 0x2022 || cp == 0x2023 || cp == 0x2043 || cp == 0x25E6 || cp == 0x2024) return '*';
    if (cp == 0x2039) return '<';
    if (cp == 0x203A) return '>';
    if (cp == 0x2044 || cp == 0x2215) return '/';

    // 4. Accented Characters - A/a
    if ((cp >= 0x00C0 && cp <= 0x00C6) || (cp >= 0x0100 && cp <= 0x0105 && (cp % 2 == 0))) return 'A';
    if ((cp >= 0x00E0 && cp <= 0x00E6) || (cp >= 0x0100 && cp <= 0x0105 && (cp % 2 == 1))) return 'a';

    // C/c
    if (cp == 0x00C7 || (cp >= 0x0106 && cp <= 0x010D && (cp % 2 == 0))) return 'C';
    if (cp == 0x00E7 || (cp >= 0x0106 && cp <= 0x010D && (cp % 2 == 1))) return 'c';

    // D/d
    if (cp == 0x00D0 || cp == 0x010E || cp == 0x0110) return 'D';
    if (cp == 0x010F || cp == 0x0111) return 'd';

    // E/e
    if ((cp >= 0x00C8 && cp <= 0x00CB) || (cp >= 0x0112 && cp <= 0x011B && (cp % 2 == 0))) return 'E';
    if ((cp >= 0x00E8 && cp <= 0x00EB) || (cp >= 0x0112 && cp <= 0x011B && (cp % 2 == 1))) return 'e';

    // G/g
    if (cp >= 0x011C && cp <= 0x0123) return (cp % 2 == 0) ? 'G' : 'g';

    // H/h
    if (cp >= 0x0124 && cp <= 0x0127) return (cp % 2 == 0) ? 'H' : 'h';

    // I/i
    if ((cp >= 0x00CC && cp <= 0x00CF) || (cp >= 0x0128 && cp <= 0x0131 && (cp % 2 == 0))) return 'I';
    if ((cp >= 0x00EC && cp <= 0x00EF) || (cp >= 0x0128 && cp <= 0x0131 && (cp % 2 == 1))) return 'i';

    // L/l
    if (cp >= 0x0139 && cp <= 0x0142) return (cp % 2 == 1) ? 'L' : 'l';

    // N/n
    if (cp == 0x00D1 || (cp >= 0x0143 && cp <= 0x014B && (cp % 2 == 1))) return 'N';
    if (cp == 0x00F1 || (cp >= 0x0143 && cp <= 0x014B && (cp % 2 == 0))) return 'n';

    // O/o
    if ((cp >= 0x00D2 && cp <= 0x00D6) || cp == 0x00D8 || (cp >= 0x014C && cp <= 0x0151 && (cp % 2 == 0)) || cp == 0x0152) return 'O';
    if ((cp >= 0x00F2 && cp <= 0x00F6) || cp == 0x00F8 || (cp >= 0x014C && cp <= 0x0151 && (cp % 2 == 1)) || cp == 0x0153) return 'o';

    // R/r
    if (cp >= 0x0154 && cp <= 0x0159) return (cp % 2 == 0) ? 'R' : 'r';

    // S/s
    if ((cp >= 0x015A && cp <= 0x0161 && (cp % 2 == 0))) return 'S';
    if ((cp >= 0x015A && cp <= 0x0161 && (cp % 2 == 1)) || cp == 0x00DF) return 's';

    // T/t
    if (cp >= 0x0162 && cp <= 0x0167) return (cp % 2 == 0) ? 'T' : 't';

    // U/u
    if ((cp >= 0x00D9 && cp <= 0x00DC) || (cp >= 0x0168 && cp <= 0x0173 && (cp % 2 == 0))) return 'U';
    if ((cp >= 0x00F9 && cp <= 0x00FC) || (cp >= 0x0168 && cp <= 0x0173 && (cp % 2 == 1))) return 'u';

    // W/w, Y/y, Z/z
    if (cp == 0x0174) return 'W';
    if (cp == 0x0175) return 'w';
    if (cp == 0x00DD || cp == 0x0176 || cp == 0x0178) return 'Y';
    if (cp == 0x00FD || cp == 0x00FF || cp == 0x0177) return 'y';
    if (cp >= 0x0179 && cp <= 0x017E) return (cp % 2 == 1) ? 'Z' : 'z';

    // Ligatures & Currency
    if (cp >= 0xFB00 && cp <= 0xFB04) return 'f';
    if (cp == 0x00A9) return 'C';
    if (cp == 0x00AE) return 'R';
    if (cp == 0x2122) return 'T';
    if (cp == 0x00B0) return 'o';
    if (cp == 0x20AC || cp == 0x00A3 || cp == 0x00A5 || cp == 0x00A2) return '$';

    return ' ';
}

static int display_draw_gfx_char_raw(int x, int y, uint8_t c, const GFXfont *font, uint8_t color) {
    const uint8_t *bitmap = NULL;
    const GFXglyph *glyph = font_get_glyph((uint32_t)c, font, &bitmap);

    if (!glyph) {
        c = '?';
        glyph = font_get_glyph((uint32_t)c, font, &bitmap);
        if (!glyph) return 0;
    }

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

void display_draw_atkinson_dithered(int dst_x, int dst_y, int target_w, int target_h, const uint8_t *gray_img, int img_w, int img_h) {
    if (!gray_img || img_w <= 0 || img_h <= 0 || target_w <= 0 || target_h <= 0) return;
    if (target_w > 200) target_w = 200;

    static int16_t err_buf[3][200];
    memset(err_buf, 0, sizeof(err_buf));

    for (int y = 0; y < target_h; y++) {
        int r0 = y % 3;
        int r1 = (y + 1) % 3;
        int r2 = (y + 2) % 3;

        memset(err_buf[r2], 0, target_w * sizeof(int16_t));
        int src_y = (y * img_h) / target_h;

        for (int x = 0; x < target_w; x++) {
            int src_x = (x * img_w) / target_w;
            int16_t old_val = (int16_t)gray_img[src_y * img_w + src_x] + err_buf[r0][x];

            if (old_val < 0) old_val = 0;
            if (old_val > 255) old_val = 255;

            uint8_t new_val = (old_val >= 128) ? 255 : 0;
            int16_t err = (old_val - new_val) / 8;

            display_draw_pixel(dst_x + x, dst_y + y, (new_val == 0) ? 0 : 1);

            if (x + 1 < target_w)                       err_buf[r0][x + 1] += err;
            if (x + 2 < target_w)                       err_buf[r0][x + 2] += err;
            if (x - 1 >= 0)                             err_buf[r1][x - 1] += err;
                                                        err_buf[r1][x]     += err;
            if (x + 1 < target_w)                       err_buf[r1][x + 1] += err;
                                                        err_buf[r2][x]     += err;
        }
    }
}

void display_draw_lockscreen(const char *book_name, const char *title, const char *author, uint32_t progress_pct) {
    display_clear_buffer();

    int cover_x = 15;
    int cover_y = 15;
    int cover_w = 145;
    int cover_h = 210;

    static uint8_t cover_gray_buf[145 * 210];
    bool has_cover = false;

    if (book_name && book_name[0] != '\0') {
        storage_get_book_metadata(book_name, NULL, 0, NULL, 0);
        has_cover = epub_extract_cover_image(book_name, cover_gray_buf, cover_w, cover_h);
    }

    if (has_cover) {
        display_draw_atkinson_dithered(cover_x, cover_y, cover_w, cover_h, cover_gray_buf, cover_w, cover_h);
        display_draw_rect(cover_x, cover_y, cover_w, cover_h, 0);
    } else {
        display_draw_rect(cover_x, cover_y, cover_w, cover_h, 0);
        display_draw_rect(cover_x + 3, cover_y + 3, cover_w - 6, cover_h - 6, 0);

        display_fill_rect(cover_x + 8, cover_y + 12, cover_w - 16, 20, 0);
        display_draw_gfx_string(cover_x + 14, cover_y + 26, "EPUB BOOK", &AmazonEmber_Medium9pt7b, 1);

        char title_buf[32];
        if (strlen(title) > 18) {
            snprintf(title_buf, sizeof(title_buf), "%.15s...", title);
        } else {
            snprintf(title_buf, sizeof(title_buf), "%s", title);
        }
        display_draw_gfx_string(cover_x + 12, cover_y + 75, title_buf, &AmazonEmber_Medium12pt7b, 0);

        if (author && author[0]) {
            char author_buf[32];
            if (strlen(author) > 20) {
                snprintf(author_buf, sizeof(author_buf), "%.17s...", author);
            } else {
                snprintf(author_buf, sizeof(author_buf), "%s", author);
            }
            display_draw_gfx_string(cover_x + 12, cover_y + 105, author_buf, &AmazonEmber_Medium9pt7b, 0);
        }
    }

    // 2. Right Half Metadata Panel (x=180 to 406)
    int right_x = 180;

    // Solid SLEEP MODE badge
    display_fill_rect(right_x, 25, 120, 22, 0);
    display_draw_gfx_string(right_x + 10, 41, "SLEEP MODE", &AmazonEmber_Medium9pt7b, 1);

    // Title
    display_draw_gfx_string(right_x, 85, title, &AmazonEmber_Medium12pt7b, 0);

    // Author
    if (author && author[0]) {
        display_draw_gfx_string(right_x, 115, author, &AmazonEmber_Medium9pt7b, 0);
    }

    // Progress percentage
    char prog_str[32];
    snprintf(prog_str, sizeof(prog_str), "Progress: %u%%", progress_pct);
    display_draw_gfx_string(right_x, 155, prog_str, &AmazonEmber_Medium9pt7b, 0);

    // Divider line
    display_fill_rect(right_x, 175, EPD_WIDTH - right_x - 10, 1, 0);

    // Wake hint
    display_draw_gfx_string(right_x, 205, "Press any button to wake", &AmazonEmber_Medium9pt7b, 0);

    // Execute full OTP refresh on e-Paper glass
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
            if (cur_y == margin_top) {
                while (*p == '\n' || *p == '\r') p++;
                continue;
            }
            p++;
            if (*p == '\n') {
                cur_y += line_height;     /* exactly 1 blank line between paragraphs */
                while (*p == '\n' || *p == '\r') p++;
            }
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
