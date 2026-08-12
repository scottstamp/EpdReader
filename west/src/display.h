#ifndef DISPLAY_H
#define DISPLAY_H

#include "config.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef enum {
    FONT_SERIF,     // Bookerly
    FONT_SANS,      // Amazon Ember
    FONT_LITERATA,  // Literata
    FONT_ATKINSON   // Atkinson Hyperlegible
} FontType;

typedef enum {
    SIZE_SMALL,     // 9pt
    SIZE_MEDIUM,    // 12pt
    SIZE_LARGE      // 18pt
} FontSize;

typedef enum {
    SPACING_COMPACT,
    SPACING_NORMAL,
    SPACING_RELAXED
} LineSpacing;

typedef enum {
    CONTRAST_NORMAL,
    CONTRAST_INVERTED
} ContrastMode;

// Display API
int display_init(void);
void display_power_on(void);
void display_power_off(void);
void display_app_clear(void);
void display_update(bool full_refresh);
void display_set_flipped(bool flipped);
bool display_is_flipped(void);

// Framebuffer drawing helpers
void display_draw_pixel(int x, int y, uint8_t color);
void display_fill_rect(int x, int y, int w, int h, uint8_t color);
void display_draw_rect(int x, int y, int w, int h, uint8_t color);
void display_draw_line(int x0, int y0, int x1, int y1, uint8_t color);
void display_draw_string(int x, int y, const char *str, uint8_t color);

// Views
void display_draw_menu(const char *header, const char **options, int count, int selected_idx);
void display_draw_menu_ext(const char *header, const char *sub_footer, const char **options, int count, int selected_idx);
void display_draw_message(const char *title, const char *msg, bool is_alert);
void display_draw_progress(const char *task, int percentage);
void display_draw_lockscreen(const char *title, const char *author, const char *chapter, const char *page, int press_count);
uint32_t display_draw_reader_page(const char *book_name, const char *text_page, uint32_t progress_pct);

// Font/Settings accessors & manipulators
FontType display_get_font(void);
void display_set_font(FontType font);
void display_cycle_font(void);

FontSize display_get_size(void);
void display_set_size(FontSize size);
void display_cycle_size(void);

LineSpacing display_get_spacing(void);
void display_set_spacing(LineSpacing spacing);
void display_cycle_spacing(void);

ContrastMode display_get_contrast(void);
void display_app_clear(void);
void display_app_set_contrast(ContrastMode mode);
void display_cycle_contrast(void);

void display_draw_battery(int percent);
void display_clear_buffer(void);

#endif // DISPLAY_H
