#ifndef DISPLAY_H
#define DISPLAY_H

#include "config.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "display_gdey037t03.h"
#include "icons.h"

typedef enum {
    FONT_SERIF,     // Bookerly
    FONT_SANS,      // Amazon Ember
    FONT_LITERATA,  // Literata
    FONT_ATKINSON,  // Atkinson Hyperlegible
    FONT_SD         // .fnt loaded from /SD:/fonts (reader text only)
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
void display_power_off(void);
void display_update(bool full_refresh);
// Make the next display_update a full refresh
void display_request_full_refresh(void);
// Portrait (240 x 416) instead of landscape for the screens drawn next; the reader,
// lock screen and carousel are laid out for landscape only
void display_set_portrait(bool portrait);
bool display_is_portrait(void);
void display_set_flipped(bool flipped);
bool display_is_flipped(void);

// Framebuffer drawing helpers
void display_draw_pixel(int x, int y, uint8_t color);
void display_fill_rect(int x, int y, int w, int h, uint8_t color);
void display_draw_rect(int x, int y, int w, int h, uint8_t color);

typedef struct {
    const Icon *icon_large;
    const Icon *icon_small;
    const char *label;
    const char *detail;   // optional second line under the focused item
} CarouselItem;

// Views
void display_draw_icon(int x, int y, const Icon *icon, uint8_t color);
// Main menu: focused item centred with its neighbours either side; Prev/Next rotate
void display_draw_carousel(const CarouselItem *items, int count, int selected);
void display_draw_menu(const char *header, const char **options, int count, int selected_idx);
// Most rows a menu shows (portrait); display_menu_rows() gives the current number
#define DISPLAY_MENU_ROWS 18
int display_menu_rows(void);
// Which options a menu of count entries shows for selected_idx, so callers can build
// labels for the visible rows only
void display_menu_window(int count, int selected_idx, int *first, int *visible);
void display_draw_menu_ext(const char *header, const char *sub_footer, const char **options, int count, int selected_idx);
// Same, without the "(n/total)" position counter in the title
void display_draw_menu_plain(const char *header, const char *sub_footer, const char **options, int count, int selected_idx);
// Plain menu whose rows can start with a checkbox: checks[i] = 1 checked, 0 unchecked, -1 none
void display_draw_checklist(const char *header, const char *sub_footer, const char **options, const int8_t *checks,
                            int count, int selected_idx);
void display_draw_message(const char *title, const char *msg, bool is_alert);
void display_draw_progress(const char *task, int percentage);
void display_draw_lockscreen(const char *book_name, const char *title, const char *author, const char *chapter_title, uint32_t progress_pct);
// Inline style state at the start of a page (styles can stay open across page breaks)
#define READER_STYLE_ITALIC    0x01
#define READER_STYLE_BOLD      0x02
#define READER_STYLE_UNDERLINE 0x04

uint32_t display_draw_reader_page(const char *book_name, const char *text_page, uint32_t progress_pct, uint8_t style_in);
// Bytes of text_page that display_draw_reader_page would fit on one page, without drawing
uint32_t display_measure_reader_page(const char *text_page);

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
void display_app_set_contrast(ContrastMode mode);
void display_cycle_contrast(void);

void display_draw_battery(int percent);
void display_clear_buffer(void);

// Refresh Waveform & Timing Settings
enum gdey037t03_refresh_profile display_get_refresh_profile(void);
void display_set_refresh_profile(enum gdey037t03_refresh_profile profile);
void display_cycle_refresh_profile(void);

uint8_t display_get_pulse_interval(void);
void display_set_pulse_interval(uint8_t interval);
void display_cycle_pulse_interval(void);

uint8_t display_get_pulse_count(void);
void display_set_pulse_count(uint8_t count);
void display_cycle_pulse_count(void);

// Custom profile's PLL frame rate in Hz; 0 = controller default (always 0 for other profiles)
uint8_t display_get_frame_rate(void);
void display_cycle_frame_rate(void);

const char *display_get_refresh_profile_name(void);
void display_set_refresh_config(enum gdey037t03_refresh_profile profile, uint8_t interval, uint8_t count,
                                uint8_t frame_rate);
void display_get_refresh_config(enum gdey037t03_refresh_profile *profile, uint8_t *interval, uint8_t *count,
                                uint8_t *frame_rate);

#endif // DISPLAY_H
