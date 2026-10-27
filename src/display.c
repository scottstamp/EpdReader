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
#include <zephyr/fs/fs.h>
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

    // Auto-detect & load custom font from SD card if present; it becomes the default reader font
    struct fs_dirent fnt_stat;
    bool sd_font = false;
    if (fs_stat("/SD:/fonts/Bookerly.fnt", &fnt_stat) == 0) {
        sd_font = font_sd_load("/SD:/fonts/Bookerly.fnt");
    } else if (fs_stat("/SD:/fonts/Arial.fnt", &fnt_stat) == 0) {
        sd_font = font_sd_load("/SD:/fonts/Arial.fnt");
    }
    if (sd_font) {
        g_font_type = FONT_SD;
    }

    return 0;
}

void display_set_refresh_config(enum gdey037t03_refresh_profile profile, uint8_t interval, uint8_t count) {
    if (!device_is_ready(epd_dev)) return;
    struct gdey037t03_refresh_config cfg = {
        .profile = profile,
        .pulse_interval = interval,
        .pulse_count = count,
    };
    gdey037t03_set_refresh_config(epd_dev, &cfg);
}

void display_get_refresh_config(enum gdey037t03_refresh_profile *profile, uint8_t *interval, uint8_t *count) {
    struct gdey037t03_refresh_config cfg = {
        .profile = GDEY037T03_PROFILE_NORMAL,
        .pulse_interval = 2,
        .pulse_count = 3,
    };
    if (device_is_ready(epd_dev)) {
        gdey037t03_get_refresh_config(epd_dev, &cfg);
    }
    *profile = cfg.profile;
    *interval = cfg.pulse_interval;
    *count = cfg.pulse_count;
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
void display_cycle_font(void) {
    if (g_font_type == FONT_SERIF) {
        g_font_type = FONT_SANS;
    } else if (g_font_type == FONT_SANS && font_sd_is_active()) {
        g_font_type = FONT_SD;
    } else {
        g_font_type = FONT_SERIF;
    }
}

FontSize display_get_size(void) { return g_font_size; }
void display_set_size(FontSize size) { g_font_size = size; }
void display_cycle_size(void) { g_font_size = (g_font_size == SIZE_SMALL) ? SIZE_MEDIUM : SIZE_SMALL; }

LineSpacing display_get_spacing(void) { return g_line_spacing; }
void display_set_spacing(LineSpacing spacing) { g_line_spacing = spacing; }
void display_cycle_spacing(void) { g_line_spacing = (LineSpacing)((g_line_spacing + 1) % 3); }

ContrastMode display_get_contrast(void) { return g_contrast_mode; }
void display_app_set_contrast(ContrastMode mode) { g_contrast_mode = mode; }
void display_cycle_contrast(void) { g_contrast_mode = (ContrastMode)((g_contrast_mode + 1) % 2); }

enum gdey037t03_refresh_profile display_get_refresh_profile(void) {
    struct gdey037t03_refresh_config cfg;
    if (device_is_ready(epd_dev) && gdey037t03_get_refresh_config(epd_dev, &cfg) == 0) {
        return cfg.profile;
    }
    return GDEY037T03_PROFILE_NORMAL;
}

void display_set_refresh_profile(enum gdey037t03_refresh_profile profile) {
    struct gdey037t03_refresh_config cfg;
    if (device_is_ready(epd_dev)) {
        gdey037t03_get_refresh_config(epd_dev, &cfg);
        cfg.profile = profile;
        gdey037t03_set_refresh_config(epd_dev, &cfg);
    }
}

void display_cycle_refresh_profile(void) {
    enum gdey037t03_refresh_profile prof = display_get_refresh_profile();
    prof = (enum gdey037t03_refresh_profile)((prof + 1) % 4);
    display_set_refresh_profile(prof);
}

uint8_t display_get_pulse_interval(void) {
    struct gdey037t03_refresh_config cfg;
    if (device_is_ready(epd_dev) && gdey037t03_get_refresh_config(epd_dev, &cfg) == 0) {
        if (cfg.profile == GDEY037T03_PROFILE_NORMAL) return 2;
        if (cfg.profile == GDEY037T03_PROFILE_FAST) return 2;
        if (cfg.profile == GDEY037T03_PROFILE_TURBO) return 1;
        return cfg.pulse_interval;
    }
    return 2;
}

void display_set_pulse_interval(uint8_t interval) {
    struct gdey037t03_refresh_config cfg;
    if (device_is_ready(epd_dev)) {
        gdey037t03_get_refresh_config(epd_dev, &cfg);
        cfg.pulse_interval = interval;
        gdey037t03_set_refresh_config(epd_dev, &cfg);
    }
}

void display_cycle_pulse_interval(void) {
    uint8_t cur = display_get_pulse_interval();
    // Cycle: 1 -> 2 -> 3 -> 4 -> 6 -> 8 -> 10 -> 12 -> 15 -> 1
    uint8_t next;
    if (cur < 4) {
        next = cur + 1;
    } else if (cur == 4) {
        next = 6;
    } else if (cur == 6) {
        next = 8;
    } else if (cur == 8) {
        next = 10;
    } else if (cur == 10) {
        next = 12;
    } else if (cur == 12) {
        next = 15;
    } else {
        next = 1;
    }
    display_set_pulse_interval(next);
}

uint8_t display_get_pulse_count(void) {
    struct gdey037t03_refresh_config cfg;
    if (device_is_ready(epd_dev) && gdey037t03_get_refresh_config(epd_dev, &cfg) == 0) {
        if (cfg.profile == GDEY037T03_PROFILE_NORMAL) return 3;
        if (cfg.profile == GDEY037T03_PROFILE_FAST) return 2;
        if (cfg.profile == GDEY037T03_PROFILE_TURBO) return 1;
        return cfg.pulse_count;
    }
    return 3;
}

void display_set_pulse_count(uint8_t count) {
    struct gdey037t03_refresh_config cfg;
    if (device_is_ready(epd_dev)) {
        gdey037t03_get_refresh_config(epd_dev, &cfg);
        cfg.pulse_count = count;
        gdey037t03_set_refresh_config(epd_dev, &cfg);
    }
}

void display_cycle_pulse_count(void) {
    uint8_t cur = display_get_pulse_count();
    uint8_t next = (cur >= 4) ? 1 : (cur + 1);
    display_set_pulse_count(next);
}

const char *display_get_refresh_profile_name(void) {
    switch (display_get_refresh_profile()) {
        case GDEY037T03_PROFILE_NORMAL: return "Normal";
        case GDEY037T03_PROFILE_FAST:   return "Fast";
        case GDEY037T03_PROFILE_TURBO:  return "Turbo";
        case GDEY037T03_PROFILE_CUSTOM: return "Custom";
        default: return "Normal";
    }
}

// NULL means the SD font (see font_get_glyph_styled)
static const GFXfont* get_active_reader_font(void) {
    if (g_font_type == FONT_SD && font_sd_is_active()) {
        return NULL;
    }
    if (g_font_type == FONT_SANS) {
        if (g_font_size == SIZE_SMALL) return &AmazonEmber_Medium9pt7b;
        return &AmazonEmber_Medium12pt7b;
    } else { // FONT_SERIF (Bookerly)
        if (g_font_size == SIZE_SMALL) return &Bookerly9pt7b;
        return &Bookerly12pt7b;
    }
}

void display_request_full_refresh(void) {
    gdey037t03_request_full_refresh(epd_dev);
}

void display_update(bool full) {
    // Partial refreshes slowly accumulate ghosting; clear it with a periodic full refresh
    static int partials_since_full;
    if (!full && ++partials_since_full >= EPD_FULL_REFRESH_INTERVAL) {
        full = true;
    }
    if (full) {
        partials_since_full = 0;
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

// Next codepoint of a NUL-terminated UTF-8 string, or 0 at its end (a sequence cut short
// by the end also counts as the end, so it is never half consumed). Malformed bytes are
// skipped one at a time as U+FFFD.
static uint32_t utf8_next(const char **pp) {
    const uint8_t *p = (const uint8_t *)*pp;
    if (!*p) return 0;

    uint32_t cp = 0;
    int bytes = 0;

    if ((*p & 0x80) == 0x00) {
        (*pp)++;
        return *p;
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
        return 0xFFFD;
    }

    for (int i = 1; i < bytes; i++) {
        if (p[i] == 0) return 0;
        if ((p[i] & 0xC0) != 0x80) {
            *pp += i;
            return 0xFFFD;
        }
        cp = (cp << 6) | (p[i] & 0x3F);
    }

    *pp += bytes;
    return cp;
}

// End of the complete UTF-8 text in s: a multi-byte sequence truncated by the end of the
// buffer is left out, so page offsets never land inside a character.
static const char *utf8_text_end(const char *s) {
    size_t len = strlen(s);
    size_t i = len;
    size_t tail = 0;
    while (i > 0 && tail < 3 && ((uint8_t)s[i - 1] & 0xC0) == 0x80) {
        i--;
        tail++;
    }
    if (i > 0) {
        uint8_t lead = (uint8_t)s[i - 1];
        size_t need = (lead >= 0xF0) ? 4 : (lead >= 0xE0) ? 3 : (lead >= 0xC0) ? 2 : 1;
        if (need > tail + 1) return s + i - 1;
    }
    return s + len;
}

// Format characters that take no space (soft hyphen, zero-width space/joiners, bidi marks, BOM)
static bool is_invisible_cp(uint32_t cp) {
    return cp == 0x00AD || (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) ||
           (cp >= 0x2060 && cp <= 0x2064) || cp == 0xFEFF;
}

// Single ASCII stand-in for a codepoint the font can't draw
static char ascii_fallback_char(uint32_t cp) {
    // 1. Direct ASCII
    if (cp >= 32 && cp <= 126) return (char)cp;

    // 2. Spaces & Line Breaks
    if (cp == 160 || cp == 0x202F || cp == 0x2028 || cp == 0x2029) return ' ';

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

    // Symbols & Currency
    if (cp == 0x00B0) return 'o';
    if (cp == 0x00A3 || cp == 0x00A5 || cp == 0x00A2) return '$';

    return ' ';
}

// ASCII stand-in for cp, up to 3 characters; returns the count
static int ascii_fallback(uint32_t cp, char out[3]) {
    const char *multi = NULL;
    switch (cp) {
    case 0x2026: multi = "..."; break;
    case 0x00C6: multi = "AE"; break;
    case 0x00E6: multi = "ae"; break;
    case 0x0152: multi = "OE"; break;
    case 0x0153: multi = "oe"; break;
    case 0x00DF: multi = "ss"; break;
    case 0x00A9: multi = "(c)"; break;
    case 0x00AE: multi = "(R)"; break;
    case 0x2122: multi = "TM"; break;
    case 0x20AC: multi = "EUR"; break;
    case 0xFB00: multi = "ff"; break;
    case 0xFB01: multi = "fi"; break;
    case 0xFB02: multi = "fl"; break;
    case 0xFB03: multi = "ffi"; break;
    case 0xFB04: multi = "ffl"; break;
    default: break;
    }
    if (multi) {
        int n = (int)strlen(multi);
        memcpy(out, multi, n);
        return n;
    }
    out[0] = ascii_fallback_char(cp);
    return 1;
}

// Glyphs that show cp in font (NULL: SD font): its own glyph when the SD font has one,
// otherwise the ASCII stand-in. Returns the glyph count (0: invisible). Layout and drawing
// both resolve through here, so measured and drawn lines always agree.
static int resolve_glyphs(const GFXfont *font, uint32_t cp, uint32_t out[3]) {
    if (cp < 0x20 || is_invisible_cp(cp)) return 0;
    if (cp <= 0x7E || (font == NULL && font_get_glyph(cp, NULL, NULL) != NULL)) {
        out[0] = cp;
        return 1;
    }
    char ascii[3];
    int n = ascii_fallback(cp, ascii);
    for (int i = 0; i < n; i++) out[i] = (uint8_t)ascii[i];
    return n;
}

static int display_draw_gfx_char_styled(int x, int y, uint32_t c, const GFXfont *font, uint8_t color, bool is_italic, bool is_bold, bool is_underline) {
    if (c < 32) return 0;

    const uint8_t *bitmap = NULL;
    bool has_style = false;
    const GFXglyph *glyph = font_get_glyph_styled((uint32_t)c, is_bold, is_italic, font, &bitmap, &has_style);

    if (!glyph) {
        c = '?';
        glyph = font_get_glyph_styled((uint32_t)c, is_bold, is_italic, font, &bitmap, &has_style);
        if (!glyph) return 0;
    }

    // Real bold/italic glyphs from an SD font need no synthesized styling
    if (has_style) {
        is_bold = false;
        is_italic = false;
    }

    uint32_t bo = glyph->bitmapOffset;
    uint8_t  w  = glyph->width;
    uint8_t  h  = glyph->height;
    int8_t   xo = glyph->xOffset;
    int8_t   yo = glyph->yOffset;

    uint8_t  bits = 0, bit = 0;

    for (int yy = 0; yy < h; yy++) {
        int slant = is_italic ? ((h - yy) / 3) : 0;
        for (int xx = 0; xx < w; xx++) {
            if (!(bit++ & 7)) {
                bits = bitmap[bo++];
            }
            if (bits & 0x80) {
                int px = x + xo + xx + slant;
                int py = y + yo + yy;
                display_draw_pixel(px, py, color);
                if (is_bold) {
                    display_draw_pixel(px + 1, py, color);
                    display_draw_pixel(px, py + 1, color);
                    display_draw_pixel(px + 1, py + 1, color);
                }
            }
            bits <<= 1;
        }
    }

    if (is_underline) {
        // y is the baseline for built-in fonts but the line top for SD fonts
        int ul_y = font ? y + 2 : y + font_sd_ascent() + 1;
        int adv = glyph->xAdvance;
        for (int ix = 0; ix < adv; ix++) {
            display_draw_pixel(x + ix, ul_y, color);
        }
    }

    return glyph->xAdvance + (is_bold ? 1 : 0);
}

// Draws len bytes of UTF-8 text with inline style codes, NUL-terminated at str[len]
static int display_draw_gfx_string_styled(int x, int y, const char *str, int len, const GFXfont *font, uint8_t color, bool *in_italic, bool *in_bold, bool *in_underline) {
    int cur_x = x;
    const char *p = str;
    const char *end = str + len;
    while (p < end) {
        uint32_t cp = utf8_next(&p);
        if (cp == 0) break;
        if (cp == '\x0E') { *in_italic = true; continue; }
        if (cp == '\x0F') { *in_italic = false; continue; }
        if (cp == '\x02') { *in_bold = true; continue; }
        if (cp == '\x03') { *in_bold = false; continue; }
        if (cp == '\x15') { *in_underline = true; continue; }
        if (cp == '\x16') { *in_underline = false; continue; }

        uint32_t glyphs[3];
        int n = resolve_glyphs(font, cp, glyphs);
        for (int i = 0; i < n; i++) {
            cur_x += display_draw_gfx_char_styled(cur_x, y, glyphs[i], font, color, *in_italic, *in_bold, *in_underline);
        }
    }
    return cur_x - x;
}

// Draws text already reduced to ASCII by fit_text
static int display_draw_gfx_string_raw(int x, int y, const char *str, const GFXfont *font, uint8_t color) {
    int cur_x = x;
    for (const char *p = str; *p; p++) {
        cur_x += display_draw_gfx_char_styled(cur_x, y, (uint8_t)*p, font, color, false, false, false);
    }
    return cur_x - x;
}

static int builtin_advance(const GFXfont *font, uint8_t c) {
    uint16_t glyph_idx = (c >= font->first && c <= font->last) ? (c - font->first) : ('?' - font->first);
    return font->glyph[glyph_idx].xAdvance;
}

// Decodes str into out as ASCII (built-in fonts have no other glyphs), shortening it
// with "..." to fit max_width. Returns the resulting width in pixels.
static int fit_text(const char *str, const GFXfont *font, int max_width, char *out, size_t out_size) {
    int out_idx = 0;
    const char *p = str;

    while (*p) {
        uint32_t cp = utf8_next(&p);
        if (cp == 0) break;
        uint32_t glyphs[3];
        int n = resolve_glyphs(font, cp, glyphs);
        if (cp < 0x20) {
            glyphs[0] = ' ';
            n = 1;
        }
        if (out_idx + n > (int)out_size - 1) break;
        for (int i = 0; i < n; i++) out[out_idx++] = (char)glyphs[i];
    }
    out[out_idx] = 0;

    int total_width = 0;
    for (int i = 0; i < out_idx; i++) {
        total_width += builtin_advance(font, (uint8_t)out[i]);
    }

    if (total_width > max_width && out_idx > 3) {
        int w = 0;
        int ellip_w = builtin_advance(font, '.') * 3;
        int cut_idx = out_idx;

        for (int i = 0; i < out_idx; i++) {
            int adv = builtin_advance(font, (uint8_t)out[i]);
            if (w + adv + ellip_w > max_width) {
                cut_idx = i;
                break;
            }
            w += adv;
        }

        out[cut_idx++] = '.';
        out[cut_idx++] = '.';
        out[cut_idx++] = '.';
        out[cut_idx] = 0;
        total_width = w + ellip_w;
    }
    return total_width;
}

static int display_draw_gfx_string(int x, int y, const char *str, const GFXfont *font, uint8_t color) {
    char decoded_buf[256];
    fit_text(str, font, EPD_WIDTH - x - 5, decoded_buf, sizeof(decoded_buf));
    return display_draw_gfx_string_raw(x, y, decoded_buf, font, color);
}

// Draws str horizontally centred on center_x, shortened to max_width if needed
static void draw_text_centered(int center_x, int y, const char *str, const GFXfont *font, int max_width, uint8_t color) {
    char decoded_buf[256];
    int w = fit_text(str, font, max_width, decoded_buf, sizeof(decoded_buf));
    display_draw_gfx_string_raw(center_x - w / 2, y, decoded_buf, font, color);
}

void display_draw_icon(int x, int y, const Icon *icon, uint8_t color) {
    int stride = (icon->width + 7) / 8;
    for (int row = 0; row < icon->height; row++) {
        const uint8_t *line = &icon->bits[row * stride];
        for (int col = 0; col < icon->width; col++) {
            if (line[col / 8] & (0x80 >> (col % 8))) {
                display_draw_pixel(x + col, y + row, color);
            }
        }
    }
}

void display_draw_carousel(const CarouselItem *items, int count, int selected) {
    display_clear_buffer();
    display_draw_battery(battery_get_percentage());

    const int center_x = EPD_WIDTH / 2;
    const CarouselItem *cur = &items[selected];

    // Neighbours: small and unframed, so the framed centre item reads as the focus
    if (count > 1) {
        const CarouselItem *prev = &items[(selected + count - 1) % count];
        const CarouselItem *next = &items[(selected + 1) % count];
        const int side_offset = 132;
        const int side_y = 92;
        display_draw_icon(center_x - side_offset - prev->icon_small->width / 2, side_y, prev->icon_small, 0);
        display_draw_icon(center_x + side_offset - next->icon_small->width / 2, side_y, next->icon_small, 0);
        draw_text_centered(center_x - side_offset, side_y + prev->icon_small->height + 18, prev->label,
                           &AmazonEmber_Medium9pt7b, 110, 0);
        draw_text_centered(center_x + side_offset, side_y + next->icon_small->height + 18, next->label,
                           &AmazonEmber_Medium9pt7b, 110, 0);
    }

    // Focused item: large icon in a rounded frame, label and one line of detail
    const int icon_y = 54;
    const int frame = 10;
    int fx = center_x - cur->icon_large->width / 2 - frame;
    int fw = cur->icon_large->width + frame * 2;
    display_draw_rect(fx, icon_y - frame, fw, cur->icon_large->height + frame * 2, 0);
    display_draw_rect(fx + 1, icon_y - frame + 1, fw - 2, cur->icon_large->height + frame * 2 - 2, 0);
    display_draw_icon(center_x - cur->icon_large->width / 2, icon_y, cur->icon_large, 0);

    int label_y = icon_y + cur->icon_large->height + frame + 26;
    draw_text_centered(center_x, label_y, cur->label, &AmazonEmber_Medium12pt7b, EPD_WIDTH - 40, 0);
    if (cur->detail && cur->detail[0]) {
        draw_text_centered(center_x, label_y + 24, cur->detail, &AmazonEmber_Medium9pt7b, EPD_WIDTH - 40, 0);
    }

    // Position dots
    const int dot = 6, gap = 10;
    int dots_w = count * dot + (count - 1) * gap;
    int dx = center_x - dots_w / 2;
    for (int i = 0; i < count; i++) {
        int x = dx + i * (dot + gap);
        if (i == selected) {
            display_fill_rect(x, EPD_HEIGHT - 16, dot, dot, 0);
        } else {
            display_draw_rect(x, EPD_HEIGHT - 16, dot, dot, 0);
        }
    }

    display_update(false);
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

    char header_buf[64];
    if (count > 6) {
        snprintf(header_buf, sizeof(header_buf), "%s (%d/%d)", title, selected_idx + 1, count);
    } else {
        snprintf(header_buf, sizeof(header_buf), "%s", title);
    }

    display_draw_gfx_string(10, 15, header_buf, &AmazonEmber_Medium9pt7b, 0);
    display_draw_battery(battery_get_percentage());
    display_fill_rect(0, 20, EPD_WIDTH, 1, 0);

    int max_visible = 10;
    int scroll_top = 0;
    if (count > max_visible) {
        if (selected_idx >= max_visible - 1) {
            scroll_top = selected_idx - (max_visible - 2);
        }
        if (scroll_top + max_visible > count) {
            scroll_top = count - max_visible;
        }
        if (scroll_top < 0) scroll_top = 0;
    }

    int visible_count = (count < max_visible) ? count : max_visible;
    int start_y = 38;
    int item_height = 21;

    for (int v = 0; v < visible_count; v++) {
        int i = scroll_top + v;
        if (i >= count) break;

        int y = start_y + (v * item_height);
        if (i == selected_idx) {
            display_fill_rect(6, y - 14, EPD_WIDTH - 12, 19, 0);
            display_draw_gfx_string(16, y, options[i], &AmazonEmber_Medium12pt7b, 1);
        } else {
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

            if (x + 1 < target_w) {
                err_buf[r0][x + 1] += err;
            }
            if (x + 2 < target_w) {
                err_buf[r0][x + 2] += err;
            }
            if (x - 1 >= 0) {
                err_buf[r1][x - 1] += err;
            }
            err_buf[r1][x] += err;
            if (x + 1 < target_w) {
                err_buf[r1][x + 1] += err;
            }
            err_buf[r2][x] += err;
        }
    }
}

void display_draw_lockscreen(const char *book_name, const char *title, const char *author, const char *chapter_title, uint32_t progress_pct) {
    display_clear_buffer();

    int cover_x = 15;
    int cover_y = 15;
    int cover_w = 145;
    int cover_h = 210;

    static uint8_t cover_gray_buf[145 * 210];
    bool has_cover = false;

    if (book_name && book_name[0] != '\0') {
        const EpubBook *epub = storage_get_epub(book_name);
        has_cover = epub && epub_extract_cover_image(epub, cover_gray_buf, cover_w, cover_h);
    }

    if (has_cover) {
        // --- LAYOUT A: WITH .COV COVER BITMAP ---
        bool is_binary = true;
        for (int i = 0; i < cover_w * cover_h; i++) {
            uint8_t v = cover_gray_buf[i];
            if (v > 10 && v < 245) {
                is_binary = false;
                break;
            }
        }

        if (is_binary) {
            for (int y = 0; y < cover_h; y++) {
                for (int x = 0; x < cover_w; x++) {
                    uint8_t val = cover_gray_buf[y * cover_w + x];
                    display_draw_pixel(cover_x + x, cover_y + y, (val < 128) ? 0 : 1);
                }
            }
        } else {
            display_draw_atkinson_dithered(cover_x, cover_y, cover_w, cover_h, cover_gray_buf, cover_w, cover_h);
        }
        display_draw_rect(cover_x, cover_y, cover_w, cover_h, 0);

        // Right Half Metadata Panel (x=175 to EPD_WIDTH)
        int right_x = 175;

        // Solid SLEEP MODE badge
        display_fill_rect(right_x, 15, 120, 22, 0);
        display_draw_gfx_string(right_x + 10, 31, "SLEEP MODE", &AmazonEmber_Medium9pt7b, 1);

        // Title
        if (title && title[0]) {
            display_draw_gfx_string(right_x, 70, title, &AmazonEmber_Medium12pt7b, 0);
        }

        // Author
        if (author && author[0]) {
            display_draw_gfx_string(right_x, 98, author, &AmazonEmber_Medium9pt7b, 0);
        }

        // Chapter
        if (chapter_title && chapter_title[0]) {
            char chap_buf[48];
            if (strlen(chapter_title) > 24) {
                snprintf(chap_buf, sizeof(chap_buf), "%.21s...", chapter_title);
            } else {
                snprintf(chap_buf, sizeof(chap_buf), "%s", chapter_title);
            }
            display_draw_gfx_string(right_x, 130, chap_buf, &AmazonEmber_Medium9pt7b, 0);
        }

        // Progress percentage
        char prog_str[32];
        snprintf(prog_str, sizeof(prog_str), "Progress: %u%%", progress_pct);
        display_draw_gfx_string(right_x, 158, prog_str, &AmazonEmber_Medium9pt7b, 0);

        // Divider line
        display_fill_rect(right_x, 178, EPD_WIDTH - right_x - 15, 1, 0);

        // Wake hint & Battery
        display_draw_gfx_string(right_x, 205, "Press button to wake", &AmazonEmber_Medium9pt7b, 0);
        display_draw_battery(battery_get_percentage());

    } else {
        // --- LAYOUT B: FULL WIDTH TEXT LOCKSCREEN (NO FAUX COVER BOX) ---
        int left_x = 20;

        // Solid SLEEP MODE badge (top left)
        display_fill_rect(left_x, 18, 120, 22, 0);
        display_draw_gfx_string(left_x + 10, 34, "SLEEP MODE", &AmazonEmber_Medium9pt7b, 1);

        // Top right battery indicator
        display_draw_battery(battery_get_percentage());

        // Full width divider line
        display_fill_rect(0, 48, EPD_WIDTH, 1, 0);

        // Title (Full Width 12pt)
        if (title && title[0]) {
            display_draw_gfx_string(left_x, 85, title, &AmazonEmber_Medium12pt7b, 0);
        } else if (book_name && book_name[0]) {
            display_draw_gfx_string(left_x, 85, book_name, &AmazonEmber_Medium12pt7b, 0);
        } else {
            display_draw_gfx_string(left_x, 85, "e-Paper Reader", &AmazonEmber_Medium12pt7b, 0);
        }

        // Author (Full Width 9pt)
        if (author && author[0]) {
            char author_buf[64];
            snprintf(author_buf, sizeof(author_buf), "By %s", author);
            display_draw_gfx_string(left_x, 115, author_buf, &AmazonEmber_Medium9pt7b, 0);
        }

        // Current Chapter (Full Width 9pt)
        if (chapter_title && chapter_title[0]) {
            display_draw_gfx_string(left_x, 145, chapter_title, &AmazonEmber_Medium9pt7b, 0);
        }

        // Progress bar & Percentage
        char prog_str[32];
        snprintf(prog_str, sizeof(prog_str), "Reading Progress: %u%%", progress_pct);
        display_draw_gfx_string(left_x, 178, prog_str, &AmazonEmber_Medium9pt7b, 0);

        // Progress Bar Line
        int bar_w = EPD_WIDTH - 40;
        display_draw_rect(left_x, 188, bar_w, 8, 0);
        int fill_w = (bar_w - 4) * progress_pct / 100;
        if (fill_w > 0) {
            display_fill_rect(left_x + 2, 190, fill_w, 4, 0);
        }

        // Wake hint bottom bar
        display_draw_gfx_string(left_x, 222, "Press any button to wake", &AmazonEmber_Medium9pt7b, 0);
    }

    display_update(true);
}

// Layout width of c, from the same glyph source used to draw it (font == NULL: SD font)
static int glyph_advance(const GFXfont *font, uint32_t c) {
    const GFXglyph *g = font_get_glyph(c, font, NULL);
    if (!g) g = font_get_glyph('?', font, NULL);
    return g ? g->xAdvance : 0;
}

// Layout width of codepoint cp, after the same glyph resolution used for drawing
static int codepoint_advance(const GFXfont *font, uint32_t cp) {
    uint32_t glyphs[3];
    int n = resolve_glyphs(font, cp, glyphs);
    int w = 0;
    for (int i = 0; i < n; i++) w += glyph_advance(font, glyphs[i]);
    return w;
}

static bool is_layout_break(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// One drawn line of source UTF-8 bytes and style codes; ~100 glyphs of up to 3 bytes fit
#define READER_LINE_BUF_SIZE 384

// Lays out one page of body text and returns the bytes of text_page it consumed.
// With draw == false nothing is rendered, so page breaks can be computed off-screen.
static uint32_t layout_reader_text(const char *text_page, bool draw, uint8_t style_in) {
    const GFXfont *active_font = get_active_reader_font();
    int font_height = active_font ? ((g_font_size == SIZE_SMALL) ? 14 : 18) : font_sd_line_height();
    int line_height = font_height + ((g_line_spacing == SPACING_COMPACT) ? 4 : ((g_line_spacing == SPACING_RELAXED) ? 10 : 7));

    int margin_top = 41; // +3px top padding below header bar
    int margin_bottom = EPD_HEIGHT - 6;
    int margin_left = 10;
    int max_width = EPD_WIDTH - 20;

    int cur_y = margin_top;
    int max_y = margin_bottom - font_height + 4;

    // Styles still open from the previous page
    bool page_italic = (style_in & READER_STYLE_ITALIC) != 0;
    bool page_bold = (style_in & READER_STYLE_BOLD) != 0;
    bool page_underline = (style_in & READER_STYLE_UNDERLINE) != 0;

    const char *p = text_page;
    const char *page_start = text_page;
    const char *end = utf8_text_end(text_page);

    while (p < end && cur_y <= max_y) {
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

        // The line keeps the source UTF-8; drawing resolves each glyph again the same way
        char line_buf[READER_LINE_BUF_SIZE];
        const int line_cap = (int)sizeof(line_buf) - 2; // room for a hyphen and the NUL
        int line_len = 0;
        int line_width = 0;
        const char *line_start_p = p;

        while (p < end && *p != '\n' && *p != '\r') {
            const char *word_start_p = p;
            int word_width = 0;

            if (*p == ' ' || *p == '\t') {
                while (*p == ' ' || *p == '\t') p++;
                int space_adv = glyph_advance(active_font, ' ');
                if (line_len > 0 && line_len < line_cap && (line_width + space_adv <= max_width)) {
                    line_buf[line_len++] = ' ';
                    line_width += space_adv;
                }
                continue;
            }

            while (p < end && !is_layout_break(*p)) {
                word_width += codepoint_advance(active_font, utf8_next(&p));
            }
            int word_bytes = (int)(p - word_start_p);

            if (line_width + word_width <= max_width && line_len + word_bytes <= line_cap) {
                memcpy(line_buf + line_len, word_start_p, word_bytes);
                line_len += word_bytes;
                line_width += word_width;
                continue;
            }

            p = word_start_p;
            if (line_len > 0) break;

            // A word wider than the whole line: split it with a hyphen, taking at least one glyph
            int hyphen_adv = codepoint_advance(active_font, '-');
            while (p < end && !is_layout_break(*p)) {
                const char *before_char = p;
                int adv = codepoint_advance(active_font, utf8_next(&p));
                int n = (int)(p - before_char);
                if ((line_width > 0 && line_width + adv + hyphen_adv > max_width) || line_len + n > line_cap) {
                    p = before_char;
                    line_buf[line_len++] = '-';
                    break;
                }
                memcpy(line_buf + line_len, before_char, n);
                line_len += n;
                line_width += adv;
            }
            break;
        }

        line_buf[line_len] = 0;
        if (line_len > 0) {
            if (draw) {
                display_draw_gfx_string_styled(margin_left, cur_y, line_buf, line_len, active_font, 0, &page_italic, &page_bold, &page_underline);
            }
            cur_y += line_height;
        } else if (p == line_start_p && p < end) {
            p++;
        }
    }

    return (uint32_t)(p - page_start);
}

uint32_t display_measure_reader_page(const char *text_page) {
    return layout_reader_text(text_page, false, 0);
}

uint32_t display_draw_reader_page(const char *book_name, const char *text_page, uint32_t progress_pct, uint8_t style_in) {
    display_clear_buffer();

    // Header Title & Battery Icon on Top Bar
    display_draw_gfx_string(10, 15, book_name, &AmazonEmber_Medium9pt7b, 0);
    display_draw_battery(battery_get_percentage());
    display_fill_rect(0, 20, EPD_WIDTH, 1, 0);

    uint32_t consumed = layout_reader_text(text_page, true, style_in);

    int bar_y = EPD_HEIGHT - 6;
    display_draw_rect(8, bar_y, EPD_WIDTH - 16, 4, 0);
    int fill_w = ((EPD_WIDTH - 16) * progress_pct) / 100;
    if (fill_w > 0) {
        display_fill_rect(8, bar_y, fill_w, 4, 0);
    }

    display_update(false);
    return consumed;
}

void display_clear_buffer(void) {
    memset(epd_framebuffer, 0xFF, EPD_BUFFER_SIZE);
}
