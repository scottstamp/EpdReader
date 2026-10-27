#include <zephyr/kernel.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_msc.h>
#include <hal/nrf_power.h>
#include "config.h"
#include "display.h"
#include "storage.h"
#include "buttons.h"
#include "ble.h"
#include "battery.h"
#include "epub.h"
#include "prefs.h"
#include <stdio.h>
#include <string.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

typedef enum {
    STATE_MENU,
    STATE_BOOK_LIST,
    STATE_CHAPTER_LIST,
    STATE_READER,
    STATE_READER_MENU,
    STATE_SETTINGS,
    STATE_REFRESH_SETTINGS,
    STATE_LOCKSCREEN,
    STATE_STATS,
    STATE_BLE_UPLOAD,
    STATE_USB_STORAGE,
    STATE_TRANSFER
} SystemState;

static SystemState current_state = STATE_MENU;
static uint32_t last_activity_time = 0;
static BookList book_list;
static int selected_book_idx = 0; // book_list.count selects "[Back]"
static char active_book[64] = "";
static uint32_t current_page_offset = 0;
static uint32_t last_page_bytes_read = 300;

// Text fetched per page; must match between drawing and measuring so page breaks agree
#define READER_PAGE_BUF_SIZE 1024
// How far before the current page to look for a paragraph start when re-paginating backwards
#define PREV_PAGE_SCAN_BYTES 3072

// How far before a page to look for the paragraph start that determines its open styles
#define STYLE_SCAN_BYTES 1024
// Reading progress is written this long after the last page turn instead of on every turn
#define PROGRESS_SAVE_DELAY_MS 3000
// Time between page turns counted as reading, at most (longer gaps are breaks)
#define READING_GAP_CAP_MS (2 * 60 * 1000)

// Start offsets of pages turned forward from, so "previous page" is exact
static uint32_t page_history[HISTORY_SIZE];
static int page_history_len = 0;

// Shared by backwards re-pagination and style scanning (main thread only)
static char scan_buf[PREV_PAGE_SCAN_BYTES + 1];

static bool progress_dirty = false;
static uint32_t progress_dirty_ms = 0;
static bool usb_msc_active = false;

static uint32_t total_reading_seconds = 0;
static uint32_t reading_mark_ms = 0;    // 0 when not in the reader
static uint32_t reading_ms_remainder = 0;

static int selected_chapter_idx = 0;

static uint32_t session_start_ms = 0;

// Home screen carousel entries, in display order
typedef enum {
    HOME_CONTINUE,
    HOME_LIBRARY,
    HOME_SETTINGS,
    HOME_TRANSFER,
    HOME_STATS,
    HOME_SLEEP,
    HOME_COUNT
} HomeItem;

typedef enum {
    READER_MENU_RESUME,
    READER_MENU_CHAPTERS,
    READER_MENU_SETTINGS,
    READER_MENU_FULL_REFRESH,
    READER_MENU_SLEEP,
    READER_MENU_HOME,
    READER_MENU_COUNT
} ReaderMenuItem;

typedef enum {
    SETTINGS_FONT,
    SETTINGS_SIZE,
    SETTINGS_SPACING,
    SETTINGS_CONTRAST,
    SETTINGS_ORIENTATION,
    SETTINGS_REFRESH,
    SETTINGS_FIRMWARE_UPDATE,
    SETTINGS_BACK,
    SETTINGS_COUNT
} SettingsItem;

typedef enum {
    TRANSFER_USB,
    TRANSFER_BLUETOOTH,
    TRANSFER_BACK,
    TRANSFER_COUNT
} TransferItem;

static int selected_menu_idx = 0; // HomeItem
static int selected_reader_menu_idx = 0;
static int selected_settings_idx = 0;
static int selected_refresh_menu_idx = 0;
static int selected_transfer_idx = 0;

// Settings opens from both the home carousel and the reader menu; Back returns there
static SystemState settings_return_state = STATE_MENU;

static SystemState pre_sleep_state = STATE_MENU;

static const EpubBook *active_epub(void) {
    return active_book[0] != '\0' ? storage_get_epub(active_book) : NULL;
}

static int chapter_index_for_offset(const EpubBook *epub, uint32_t offset) {
    for (int i = 0; i < epub->chapter_count; i++) {
        uint32_t start_off = epub->chapters[i].cum_offset;
        uint32_t end_off = start_off + epub->chapters[i].text_length;
        if (offset >= start_off && offset < end_off) {
            return i;
        }
    }
    return 0;
}

static void format_chapter_title(const EpubBook *epub, int idx, char *out, size_t out_size) {
    if (epub->chapters[idx].title[0] != '\0') {
        snprintf(out, out_size, "%s", epub->chapters[idx].title);
    } else {
        snprintf(out, out_size, "Chapter %d", idx + 1);
    }
}

// First open of an EPUB converts it to plain text; each progress draw is a panel refresh, so keep them sparse
static void on_book_prep_progress(int percent) {
    static int next_draw_pct;
    if (percent == 0) next_draw_pct = 0;
    if (percent >= next_draw_pct && percent < 100) {
        display_draw_progress("Preparing book (first open)...", percent);
        next_draw_pct = percent + 25;
    }
}

static void page_history_push(uint32_t offset) {
    if (page_history_len == HISTORY_SIZE) {
        memmove(page_history, page_history + 1, (HISTORY_SIZE - 1) * sizeof(page_history[0]));
        page_history_len--;
    }
    page_history[page_history_len++] = offset;
}

// Call whenever page breaks change (new book, chapter jump, font/size/spacing change)
static void page_history_clear(void) {
    page_history_len = 0;
}

// Without history (after resume or a jump) the previous page boundary is unknown, so lay out
// pages forward from a nearby paragraph start until reaching the current page.
static uint32_t find_previous_page_start(uint32_t offset) {
    if (offset == 0) return 0;

    uint32_t scan_start = (offset > PREV_PAGE_SCAN_BYTES) ? offset - PREV_PAGE_SCAN_BYTES : 0;
    uint32_t got = 0;
    if (!storage_read_book_page(active_book, scan_start, scan_buf, offset - scan_start + 1, &got) || got == 0) {
        return scan_start;
    }

    uint32_t anchor = scan_start;
    if (scan_start > 0) {
        // Prefer a paragraph break, else a line break, else a space, so layout starts on a word
        char *para = strstr(scan_buf, "\n\n");
        char *line = strchr(scan_buf, '\n');
        char *space = strchr(scan_buf, ' ');
        char *cut = para ? para + 2 : (line ? line + 1 : (space ? space + 1 : scan_buf));
        anchor = scan_start + (uint32_t)(cut - scan_buf);
    }

    static char page_buf[READER_PAGE_BUF_SIZE];
    uint32_t page_start = anchor;
    for (int guard = 0; guard < 16 && page_start < offset; guard++) {
        if (!storage_read_book_page(active_book, page_start, page_buf, sizeof(page_buf), &got) || got == 0) {
            break;
        }
        uint32_t consumed = display_measure_reader_page(page_buf);
        if (consumed == 0 || page_start + consumed >= offset) {
            break;
        }
        page_start += consumed;
    }
    return page_start;
}

// Bytes the page starting at offset would show, without drawing it (0 at end of book)
static uint32_t measure_page_at(uint32_t offset) {
    static char page_buf[READER_PAGE_BUF_SIZE];
    uint32_t got = 0;
    if (!storage_read_book_page(active_book, offset, page_buf, sizeof(page_buf), &got) || got == 0) {
        return 0;
    }
    return display_measure_reader_page(page_buf);
}

// Advances up to `pages` pages (several when presses queued up during a refresh)
static void turn_pages_forward(int pages) {
    uint32_t book_size = storage_get_book_size(active_book);
    for (int i = 0; i < pages; i++) {
        uint32_t len = (i == 0) ? last_page_bytes_read : measure_page_at(current_page_offset);
        if (len == 0 || current_page_offset + len >= book_size) {
            break; // stay on the last page
        }
        page_history_push(current_page_offset);
        current_page_offset += len;
    }
}

static void turn_pages_back(int pages) {
    for (int i = 0; i < pages && current_page_offset > 0; i++) {
        if (page_history_len > 0) {
            current_page_offset = page_history[--page_history_len];
        } else {
            current_page_offset = find_previous_page_start(current_page_offset);
        }
    }
}

// Inline styles (e.g. an italic passage) can be open where a page starts. Styles don't
// span paragraphs, so replay the style codes from the paragraph containing offset.
static uint8_t style_at_offset(uint32_t offset) {
    if (offset == 0) return 0;

    uint32_t scan_start = (offset > STYLE_SCAN_BYTES) ? offset - STYLE_SCAN_BYTES : 0;
    uint32_t got = 0;
    if (!storage_read_book_page(active_book, scan_start, scan_buf, offset - scan_start + 1, &got) || got == 0) {
        return 0;
    }

    const char *from = scan_buf;
    for (const char *para = strstr(scan_buf, "\n\n"); para; para = strstr(para + 2, "\n\n")) {
        from = para + 2;
    }

    uint8_t style = 0;
    for (const char *c = from; *c; c++) {
        switch (*c) {
            case '\x0E': style |= READER_STYLE_ITALIC; break;
            case '\x0F': style &= ~READER_STYLE_ITALIC; break;
            case '\x02': style |= READER_STYLE_BOLD; break;
            case '\x03': style &= ~READER_STYLE_BOLD; break;
            case '\x15': style |= READER_STYLE_UNDERLINE; break;
            case '\x16': style &= ~READER_STYLE_UNDERLINE; break;
            default: break;
        }
    }
    return style;
}

static void mark_progress_dirty(void) {
    progress_dirty = true;
    progress_dirty_ms = k_uptime_get_32();
}

static void flush_progress(void) {
    if (progress_dirty && !usb_msc_active && active_book[0] != '\0') {
        storage_write_progress(active_book, current_page_offset);
    }
    progress_dirty = false;
}

// Counts time spent in the reader; gaps longer than READING_GAP_CAP_MS count as a break
static void update_reading_time(bool still_reading) {
    uint32_t now = k_uptime_get_32();
    if (reading_mark_ms != 0) {
        uint32_t elapsed = MIN(now - reading_mark_ms, READING_GAP_CAP_MS);
        reading_ms_remainder += elapsed;
        total_reading_seconds += reading_ms_remainder / 1000;
        reading_ms_remainder %= 1000;
    }
    reading_mark_ms = still_reading ? MAX(now, 1) : 0;
}

static void save_prefs(void) {
    enum gdey037t03_refresh_profile profile;
    uint8_t interval, count;
    display_get_refresh_config(&profile, &interval, &count);

    Prefs prefs = {
        .font = (uint8_t)display_get_font(),
        .size = (uint8_t)display_get_size(),
        .spacing = (uint8_t)display_get_spacing(),
        .contrast = (uint8_t)display_get_contrast(),
        .flipped = display_is_flipped() ? 1 : 0,
        .refresh_profile = (uint8_t)profile,
        .pulse_interval = interval,
        .pulse_count = count,
        .reading_seconds = total_reading_seconds,
    };
    prefs_save(&prefs);
}

static void load_prefs(void) {
    Prefs prefs;
    if (prefs_load(&prefs)) {
        if (prefs.font <= FONT_SD) display_set_font((FontType)prefs.font);
        if (prefs.size <= SIZE_MEDIUM) display_set_size((FontSize)prefs.size);
        if (prefs.spacing <= SPACING_RELAXED) display_set_spacing((LineSpacing)prefs.spacing);
        if (prefs.contrast <= CONTRAST_INVERTED) display_app_set_contrast((ContrastMode)prefs.contrast);
        display_set_flipped(prefs.flipped != 0);
        if (prefs.refresh_profile <= GDEY037T03_PROFILE_CUSTOM) {
            display_set_refresh_config((enum gdey037t03_refresh_profile)prefs.refresh_profile,
                                       prefs.pulse_interval, prefs.pulse_count);
        }
        total_reading_seconds = prefs.reading_seconds;
    } else {
        // First boot with prefs: carry over what older firmware kept on the SD card
        int profile;
        uint8_t interval, count;
        if (storage_read_refresh_settings(&profile, &interval, &count) &&
            profile >= 0 && profile <= GDEY037T03_PROFILE_CUSTOM) {
            display_set_refresh_config((enum gdey037t03_refresh_profile)profile, interval, count);
        }
        storage_read_stats(&total_reading_seconds);
        save_prefs();
    }
    buttons_set_swapped(display_is_flipped());
}

static void format_session_time(uint32_t sec, char *out_buf, size_t buf_size) {
    uint32_t mins = sec / 60;
    uint32_t s = sec % 60;
    snprintf(out_buf, buf_size, "Session: %um %us", mins, s);
}

static void enter_deep_sleep(void) {
    LOG_INF("Entering System OFF deep sleep...");
    update_reading_time(false);
    flush_progress();
    save_prefs();
    display_power_off();
    buttons_configure_sleep_wake();
    k_msleep(100);
    sys_poweroff();
}

static void reboot_to_bootloader(void) {
    LOG_INF("Rebooting into Adafruit UF2 Bootloader...");
    nrf_power_gpregret_set(NRF_POWER, 0, 0x57);
    sys_reboot(SYS_REBOOT_WARM);
}

static int wrap_index(int idx, int delta, int count) {
    return (idx + delta + count) % count;
}

static uint32_t book_progress_percent(void) {
    uint32_t file_size = storage_get_book_size(active_book);
    uint32_t pct = (file_size > 0) ? ((current_page_offset * 100) / file_size) : 0;
    return MIN(pct, 100);
}

static void load_library(void) {
    storage_list_sd_books(&book_list);
    selected_book_idx = 0;
    for (int i = 0; i < book_list.count; i++) {
        if (strcmp(book_list_name(&book_list, i), active_book) == 0) {
            selected_book_idx = i;
            break;
        }
    }
}

static void open_book(const char *book_name) {
    flush_progress(); // the previous book's pending position
    snprintf(active_book, sizeof(active_book), "%s", book_name);
    current_page_offset = 0;
    uint32_t saved_offset;
    if (storage_read_book_progress(active_book, &saved_offset) &&
        saved_offset < storage_get_book_size(active_book)) {
        current_page_offset = saved_offset;
    }
    page_history_clear();
    // Moves the book to the top of the progress file, making it the one to continue
    storage_write_progress(active_book, current_page_offset);
    progress_dirty = false;
}

static void render_home(void) {
    char continue_detail[80] = "No book opened yet";
    if (active_book[0] != '\0') {
        char title[64];
        storage_get_book_display_title(active_book, title, sizeof(title));
        snprintf(continue_detail, sizeof(continue_detail), "%s - %u%%", title, book_progress_percent());
    }

    char stats_detail[40];
    snprintf(stats_detail, sizeof(stats_detail), "%uh %02um read in total",
             total_reading_seconds / 3600, (total_reading_seconds / 60) % 60);

    const CarouselItem items[HOME_COUNT] = {
        [HOME_CONTINUE] = { &icon_continue_large, &icon_continue_small, "Continue", continue_detail },
        [HOME_LIBRARY]  = { &icon_library_large, &icon_library_small, "Library", "Books on the SD card" },
        [HOME_SETTINGS] = { &icon_settings_large, &icon_settings_small, "Settings", "Text, display, firmware" },
        [HOME_TRANSFER] = { &icon_transfer_large, &icon_transfer_small, "Transfer", "USB drive or Bluetooth" },
        [HOME_STATS]    = { &icon_stats_large, &icon_stats_small, "Stats", stats_detail },
        [HOME_SLEEP]    = { &icon_sleep_large, &icon_sleep_small, "Sleep", "Lock screen and power off" },
    };
    display_draw_carousel(items, HOME_COUNT, selected_menu_idx);
}

static void render_settings(void) {
    FontType f = display_get_font();
    const char *f_name = (f == FONT_SANS) ? "Sans (Ember)" :
                         (f == FONT_SD) ? "SD card (.fnt)" : "Serif (Bookerly)";
    FontSize sz = display_get_size();
    LineSpacing sp = display_get_spacing();

    char font_str[40], size_str[32], spacing_str[32], contrast_str[32], orient_str[32], refresh_str[32];
    snprintf(font_str, sizeof(font_str), "Font: %s", f_name);
    snprintf(size_str, sizeof(size_str), "Size: %s", (sz == SIZE_SMALL) ? "Small (9pt)" : "Medium (12pt)");
    snprintf(spacing_str, sizeof(spacing_str), "Spacing: %s",
             (sp == SPACING_COMPACT) ? "Compact" : ((sp == SPACING_RELAXED) ? "Relaxed" : "Normal"));
    snprintf(contrast_str, sizeof(contrast_str), "Contrast: %s",
             (display_get_contrast() == CONTRAST_INVERTED) ? "Inverted" : "Normal");
    snprintf(orient_str, sizeof(orient_str), "Orientation: %s", display_is_flipped() ? "Flipped" : "Normal");
    snprintf(refresh_str, sizeof(refresh_str), "Refresh: %s", display_get_refresh_profile_name());

    const char *options[SETTINGS_COUNT] = {
        [SETTINGS_FONT] = font_str,
        [SETTINGS_SIZE] = size_str,
        [SETTINGS_SPACING] = spacing_str,
        [SETTINGS_CONTRAST] = contrast_str,
        [SETTINGS_ORIENTATION] = orient_str,
        [SETTINGS_REFRESH] = refresh_str,
        [SETTINGS_FIRMWARE_UPDATE] = "Firmware update (bootloader)",
        [SETTINGS_BACK] = "[Back]",
    };
    display_draw_menu("Settings", options, SETTINGS_COUNT, selected_settings_idx);
}

static void render_current_state(void) {
    switch (current_state) {
        case STATE_MENU:
            render_home();
            break;

        case STATE_BOOK_LIST: {
            const char *ptrs[MAX_BOOKS + 1];
            for (int i = 0; i < book_list.count; i++) ptrs[i] = book_list_label(&book_list, i);
            ptrs[book_list.count] = "[Back]";
            const char *footer = (book_list.count == 0) ? "No books found. Add .txt/.epub files to /BOOKS" : NULL;
            display_draw_menu_ext("Library", footer, ptrs, book_list.count + 1, selected_book_idx);
            break;
        }

        case STATE_READER: {
            if (session_start_ms == 0) {
                session_start_ms = k_uptime_get_32();
            }
            update_reading_time(true);

            const EpubBook *epub = active_epub();

            char chapter_header[64] = "Reader";
            if (epub && epub->chapter_count > 0) {
                int cur_idx = chapter_index_for_offset(epub, current_page_offset);
                format_chapter_title(epub, cur_idx, chapter_header, sizeof(chapter_header));
            } else {
                storage_get_book_display_title(active_book, chapter_header, sizeof(chapter_header));
            }

            char page_buf[READER_PAGE_BUF_SIZE] = {0};
            uint32_t bytes_read = 0;

            uint32_t t_start = k_uptime_get_32();
            bool ok = storage_read_book_page(active_book, current_page_offset, page_buf, sizeof(page_buf), &bytes_read);
            uint32_t t_read = k_uptime_get_32();
            if (!ok || bytes_read == 0) {
                display_draw_message(chapter_header, "Could not read book file!\nCheck SD card", true);
            } else {
                uint32_t file_size = storage_get_book_size(active_book);
                uint32_t progress_pct = (file_size > 0) ? ((current_page_offset * 100) / file_size) : 0;
                if (progress_pct > 100) progress_pct = 100;

                uint8_t style = style_at_offset(current_page_offset);
                uint32_t t_style = k_uptime_get_32();

                last_page_bytes_read = display_draw_reader_page(chapter_header, page_buf, progress_pct, style);
                if (last_page_bytes_read == 0) last_page_bytes_read = 300;

                // The panel driver logs its own refresh breakdown just before this
                LOG_INF("Page turn: read %u ms, style scan %u ms, layout+refresh %u ms, total %u ms",
                        t_read - t_start, t_style - t_read, k_uptime_get_32() - t_style,
                        k_uptime_get_32() - t_start);
            }
            break;
        }

        case STATE_READER_MENU: {
            uint32_t progress_pct = book_progress_percent();

            char display_title[64];
            storage_get_book_display_title(active_book, display_title, sizeof(display_title));

            char header_str[64];
            snprintf(header_str, sizeof(header_str), "%.48s (%u%%)", display_title[0] ? display_title : "Reader", progress_pct);

            char session_str[32];
            uint32_t session_sec = (session_start_ms > 0) ? ((k_uptime_get_32() - session_start_ms) / 1000) : 0;
            format_session_time(session_sec, session_str, sizeof(session_str));

            const char *reader_options[READER_MENU_COUNT] = {
                [READER_MENU_RESUME] = "Resume reading",
                [READER_MENU_CHAPTERS] = "Chapters",
                [READER_MENU_SETTINGS] = "Settings",
                [READER_MENU_FULL_REFRESH] = "Clear ghosting (full refresh)",
                [READER_MENU_SLEEP] = "Sleep",
                [READER_MENU_HOME] = "Home",
            };

            display_draw_menu_ext(header_str, session_str, reader_options, READER_MENU_COUNT, selected_reader_menu_idx);
            break;
        }

        case STATE_CHAPTER_LIST: {
            const EpubBook *epub = active_epub();
            const char *chap_titles[MAX_EPUB_CHAPTERS];
            int count = epub ? epub->chapter_count : 0;
            if (count > MAX_EPUB_CHAPTERS) count = MAX_EPUB_CHAPTERS;
            for (int i = 0; i < count; i++) {
                chap_titles[i] = epub->chapters[i].title;
            }
            display_draw_menu_ext("Select Chapter", NULL, chap_titles, count, selected_chapter_idx);
            break;
        }

        case STATE_SETTINGS:
            render_settings();
            break;

        case STATE_REFRESH_SETTINGS: {
            char prof_str[32];
            snprintf(prof_str, sizeof(prof_str), "Profile: %s", display_get_refresh_profile_name());

            char int_str[32];
            snprintf(int_str, sizeof(int_str), "Pulse Interval: %u", display_get_pulse_interval());

            char cnt_str[32];
            snprintf(cnt_str, sizeof(cnt_str), "Pulse Count: %u", display_get_pulse_count());

            enum gdey037t03_refresh_profile prof = display_get_refresh_profile();
            int menu_count = 0;
            const char *refresh_options[5];

            refresh_options[menu_count++] = prof_str;
            if (prof == GDEY037T03_PROFILE_CUSTOM) {
                refresh_options[menu_count++] = int_str;
                refresh_options[menu_count++] = cnt_str;
            }
            refresh_options[menu_count++] = "[Back]";

            const char *help_footer = NULL;
            if (prof == GDEY037T03_PROFILE_NORMAL) {
                help_footer = "Normal: Timing 2, 3 pulses - crisp & fast";
            } else if (prof == GDEY037T03_PROFILE_FAST) {
                help_footer = "Fast: Timing 2, 1 pulse - ~30 FPS high-speed";
            } else if (prof == GDEY037T03_PROFILE_TURBO) {
                help_footer = "Turbo: Timing 1, 1 pulse - ~45 FPS ultra-speed";
            } else {
                help_footer = "Custom: Tune pulse interval & pulse count";
            }

            display_draw_menu_ext("Refresh Settings", help_footer, refresh_options, menu_count, selected_refresh_menu_idx);
            break;
        }

        case STATE_LOCKSCREEN: {
            char title[64] = {0};
            char author[64] = {0};
            if (active_book[0] != '\0') {
                storage_get_book_metadata(active_book, title, sizeof(title), author, sizeof(author));
            } else {
                snprintf(title, sizeof(title), "e-Paper Reader");
                snprintf(author, sizeof(author), "nRF52840 Zephyr");
            }

            char chapter_header[64] = "";
            const EpubBook *epub = active_epub();
            if (epub && epub->chapter_count > 0) {
                int cur_idx = chapter_index_for_offset(epub, current_page_offset);
                format_chapter_title(epub, cur_idx, chapter_header, sizeof(chapter_header));
            }

            uint32_t file_size = active_book[0] ? storage_get_book_size(active_book) : 0;
            uint32_t progress_pct = (file_size > 0) ? ((current_page_offset * 100) / file_size) : 0;
            if (progress_pct > 100) progress_pct = 100;

            display_draw_lockscreen(active_book, title, author, chapter_header, progress_pct);

            // Save pre-sleep state to storage so wake-up restores exact view
            storage_write_sleep_state((int)pre_sleep_state, selected_menu_idx, active_book);

            // Enter deep sleep immediately after displaying lockscreen
            enter_deep_sleep();
            break;
        }

        case STATE_STATS: {
            char msg[64];
            snprintf(msg, sizeof(msg), "Total reading time: %uh %02um",
                     total_reading_seconds / 3600, (total_reading_seconds / 60) % 60);
            display_draw_message("Reading Stats", msg, false);
            break;
        }

        case STATE_BLE_UPLOAD:
            display_draw_message("BLE Upload", "Advertising as:\nnRF EPD Reader", false);
            break;

        case STATE_TRANSFER: {
            const char *options[TRANSFER_COUNT] = {
                [TRANSFER_USB] = "USB drive (SD card)",
                [TRANSFER_BLUETOOTH] = "Bluetooth upload",
                [TRANSFER_BACK] = "[Back]",
            };
            display_draw_menu("Transfer", options, TRANSFER_COUNT, selected_transfer_idx);
            break;
        }

        case STATE_USB_STORAGE:
            if (usb_msc_active) {
                display_draw_message("USB Drive Mode", "SD card is shown on the computer. Eject it there, then press Select.", false);
            } else {
                display_draw_message("USB Drive Mode", "Could not share the SD card over USB. Press Select.", true);
            }
            break;
    }
}

#if defined(CONFIG_USB_DEVICE_STACK_NEXT)
USBD_DEFINE_MSC_LUN(sd, "SD", "NiceKeyboards", "EPD Reader", "1.00");

USBD_DEVICE_DEFINE(epd_usbd,
                   DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
                   0x2FE3, 0x0004);
USBD_DESC_LANG_DEFINE(epd_lang);
USBD_DESC_MANUFACTURER_DEFINE(epd_mfr, "NiceKeyboards");
USBD_DESC_PRODUCT_DEFINE(epd_product, "nRF EPD Reader");
USBD_DESC_CONFIG_DEFINE(epd_fs_cfg_desc, "FS Configuration");
USBD_CONFIGURATION_DEFINE(epd_fs_config, 0, 125, &epd_fs_cfg_desc);

// The serial console is always available; the SD card is only offered as a USB drive in
// USB Transfer mode, while its FAT volume is unmounted here. Otherwise the computer and
// the firmware could both write the filesystem and corrupt it.
static int usb_start(bool with_msc) {
    int err = usbd_add_descriptor(&epd_usbd, &epd_lang);
    if (!err) err = usbd_add_descriptor(&epd_usbd, &epd_mfr);
    if (!err) err = usbd_add_descriptor(&epd_usbd, &epd_product);
    if (!err) err = usbd_add_configuration(&epd_usbd, USBD_SPEED_FS, &epd_fs_config);
    if (!err) err = usbd_register_class(&epd_usbd, "cdc_acm_0", USBD_SPEED_FS, 1);
    if (!err && with_msc) err = usbd_register_class(&epd_usbd, "msc_0", USBD_SPEED_FS, 1);
    if (err) {
        LOG_ERR("Failed to configure USB device (err %d)", err);
        return err;
    }
    usbd_device_set_code_triple(&epd_usbd, USBD_SPEED_FS, USB_BCC_MISCELLANEOUS, 0x02, 0x01);
    err = usbd_init(&epd_usbd);
    if (!err) err = usbd_enable(&epd_usbd);
    if (err) {
        LOG_ERR("Failed to start USB device (err %d)", err);
    }
    return err;
}

// Re-enumerates with or without the mass storage class
static int usb_restart(bool with_msc) {
    usbd_disable(&epd_usbd);
    usbd_shutdown(&epd_usbd);
    return usb_start(with_msc);
}
#endif

static void usb_storage_enter(void) {
    flush_progress();
    save_prefs();
#if defined(CONFIG_USB_DEVICE_STACK_NEXT)
    if (storage_release_for_usb() == 0 && usb_restart(true) == 0) {
        usb_msc_active = true;
    }
#endif
}

static void usb_storage_exit(void) {
#if defined(CONFIG_USB_DEVICE_STACK_NEXT)
    if (usb_msc_active) {
        usb_msc_active = false;
        usb_restart(false);
    }
    // Remount even after a failed hand-over, in case the unmount went through
    storage_reclaim_from_usb();
#endif
}

int main(void) {
    nrf_power_gpregret_set(NRF_POWER, 0, 0);

    if ((NRF_UICR->NFCPINS & UICR_NFCPINS_PROTECT_Msk) == (UICR_NFCPINS_PROTECT_NFC << UICR_NFCPINS_PROTECT_Pos)) {
        LOG_INF("Unlocking P0.09 (CS) and P0.10 (MOSI) from NFC mode in UICR...");
        NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Wen << NVMC_CONFIG_WEN_Pos;
        while (NRF_NVMC->READY == NVMC_READY_READY_Busy);
        NRF_UICR->NFCPINS &= ~UICR_NFCPINS_PROTECT_Msk;
        while (NRF_NVMC->READY == NVMC_READY_READY_Busy);
        NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren << NVMC_CONFIG_WEN_Pos;
        while (NRF_NVMC->READY == NVMC_READY_READY_Busy);
        sys_reboot(SYS_REBOOT_COLD);
    }

#if defined(CONFIG_USB_DEVICE_STACK_NEXT)
    usb_start(false);
#endif

    LOG_INF("Starting nRF e-Paper Book Reader (Zephyr RTOS)");

    // Mount the SD card first: display_init loads the SD font from it
    storage_init();
    display_init();
    epub_set_progress_cb(on_book_prep_progress);
    buttons_init();
    battery_init();
    load_prefs();

    int restored_state = -1;
    int restored_idx = 0;
    char restored_book[64] = {0};

    if (storage_read_sleep_state(&restored_state, &restored_idx, restored_book) && restored_state >= 0) {
        if (restored_book[0] != '\0') {
            snprintf(active_book, sizeof(active_book), "%s", restored_book);
            uint32_t saved_offset = 0;
            if (storage_read_book_progress(active_book, &saved_offset)) {
                current_page_offset = saved_offset;
            }
        }
        if (restored_state == (int)STATE_READER || restored_state == (int)STATE_READER_MENU) {
            current_state = STATE_READER;
        } else if (restored_state == (int)STATE_BOOK_LIST) {
            current_state = STATE_BOOK_LIST;
            load_library();
        } else {
            current_state = STATE_MENU;
            selected_menu_idx = (restored_idx >= 0 && restored_idx < HOME_COUNT) ? restored_idx : 0;
        }
        storage_clear_sleep_state();
    } else {
        char saved_book[64] = "";
        uint32_t saved_offset = 0;
        if (storage_read_progress(saved_book, &saved_offset) && saved_book[0] != '\0') {
            snprintf(active_book, sizeof(active_book), "%s", saved_book);
            current_page_offset = saved_offset;
        }
        current_state = STATE_MENU;
    }

    render_current_state();
    last_activity_time = k_uptime_get_32();

    while (1) {
        // Sleep until a button event, the pending progress save, or the auto-sleep deadline
        uint32_t now = k_uptime_get_32();
        int32_t timeout_ms = -1;
        if (current_state != STATE_USB_STORAGE) {
            uint32_t idle = now - last_activity_time;
            timeout_ms = (idle >= AUTO_SLEEP_MS) ? 0 : (int32_t)(AUTO_SLEEP_MS - idle);
        }
        if (progress_dirty) {
            uint32_t since = now - progress_dirty_ms;
            int32_t save_in = (since >= PROGRESS_SAVE_DELAY_MS) ? 0 : (int32_t)(PROGRESS_SAVE_DELAY_MS - since);
            if (timeout_ms < 0 || save_in < timeout_ms) timeout_ms = save_in;
        }

        ButtonEvent evt = buttons_wait(timeout_ms);
        if (evt != BTN_EVENT_NONE) {
            last_activity_time = k_uptime_get_32();
        }

        switch (evt) {
            case BTN_EVENT_PREV_CLICK:
            case BTN_EVENT_NEXT_CLICK: {
                int step = (evt == BTN_EVENT_NEXT_CLICK) ? 1 : -1;
                if (current_state == STATE_MENU) {
                    selected_menu_idx = wrap_index(selected_menu_idx, step, HOME_COUNT);
                    render_current_state();
                } else if (current_state == STATE_BOOK_LIST) {
                    if (book_list.count > 0) {
                        selected_book_idx = wrap_index(selected_book_idx, step, book_list.count + 1);
                        render_current_state();
                    }
                } else if (current_state == STATE_READER) {
                    if (step > 0) {
                        turn_pages_forward(1 + buttons_take_repeats(BTN_EVENT_NEXT_CLICK));
                    } else {
                        turn_pages_back(1 + buttons_take_repeats(BTN_EVENT_PREV_CLICK));
                    }
                    mark_progress_dirty();
                    render_current_state();
                } else if (current_state == STATE_CHAPTER_LIST) {
                    const EpubBook *epub = active_epub();
                    if (epub && epub->chapter_count > 0) {
                        selected_chapter_idx = wrap_index(selected_chapter_idx, step, epub->chapter_count);
                        render_current_state();
                    }
                } else if (current_state == STATE_READER_MENU) {
                    selected_reader_menu_idx = wrap_index(selected_reader_menu_idx, step, READER_MENU_COUNT);
                    render_current_state();
                } else if (current_state == STATE_SETTINGS) {
                    selected_settings_idx = wrap_index(selected_settings_idx, step, SETTINGS_COUNT);
                    render_current_state();
                } else if (current_state == STATE_TRANSFER) {
                    selected_transfer_idx = wrap_index(selected_transfer_idx, step, TRANSFER_COUNT);
                    render_current_state();
                } else if (current_state == STATE_REFRESH_SETTINGS) {
                    int max_opts = (display_get_refresh_profile() == GDEY037T03_PROFILE_CUSTOM) ? 4 : 2;
                    selected_refresh_menu_idx = wrap_index(selected_refresh_menu_idx, step, max_opts);
                    render_current_state();
                }
                break;
            }

            case BTN_EVENT_SELECT_CLICK:
                if (current_state == STATE_MENU) {
                    switch ((HomeItem)selected_menu_idx) {
                        case HOME_CONTINUE:
                            if (active_book[0] != '\0') {
                                current_state = STATE_READER;
                            } else {
                                display_draw_message("Continue", "No book opened yet. Pick one in the Library.", true);
                                k_msleep(1500);
                            }
                            break;
                        case HOME_LIBRARY:
                            load_library();
                            current_state = STATE_BOOK_LIST;
                            break;
                        case HOME_SETTINGS:
                            settings_return_state = STATE_MENU;
                            selected_settings_idx = 0;
                            current_state = STATE_SETTINGS;
                            break;
                        case HOME_TRANSFER:
                            selected_transfer_idx = 0;
                            current_state = STATE_TRANSFER;
                            break;
                        case HOME_STATS:
                            current_state = STATE_STATS;
                            break;
                        case HOME_SLEEP:
                            pre_sleep_state = STATE_MENU;
                            current_state = STATE_LOCKSCREEN;
                            break;
                        default:
                            break;
                    }
                    render_current_state();
                } else if (current_state == STATE_BOOK_LIST) {
                    if (selected_book_idx < book_list.count) {
                        open_book(book_list_name(&book_list, selected_book_idx));
                        current_state = STATE_READER;
                    } else {
                        current_state = STATE_MENU;
                    }
                    render_current_state();
                } else if (current_state == STATE_READER) {
                    selected_reader_menu_idx = 0;
                    current_state = STATE_READER_MENU;
                    render_current_state();
                } else if (current_state == STATE_READER_MENU) {
                    switch ((ReaderMenuItem)selected_reader_menu_idx) {
                        case READER_MENU_RESUME:
                            current_state = STATE_READER;
                            break;
                        case READER_MENU_CHAPTERS: {
                            const EpubBook *epub = active_epub();
                            selected_chapter_idx = epub ? chapter_index_for_offset(epub, current_page_offset) : 0;
                            current_state = STATE_CHAPTER_LIST;
                            break;
                        }
                        case READER_MENU_SETTINGS:
                            settings_return_state = STATE_READER_MENU;
                            selected_settings_idx = 0;
                            current_state = STATE_SETTINGS;
                            break;
                        case READER_MENU_FULL_REFRESH:
                            display_request_full_refresh(); // applies to the page drawn next
                            current_state = STATE_READER;
                            break;
                        case READER_MENU_SLEEP:
                            pre_sleep_state = STATE_READER;
                            current_state = STATE_LOCKSCREEN;
                            break;
                        case READER_MENU_HOME:
                            current_state = STATE_MENU;
                            break;
                        default:
                            break;
                    }
                    render_current_state();
                } else if (current_state == STATE_CHAPTER_LIST) {
                    const EpubBook *epub = active_epub();
                    if (epub && selected_chapter_idx >= 0 && selected_chapter_idx < epub->chapter_count) {
                        current_page_offset = epub->chapters[selected_chapter_idx].cum_offset;
                        page_history_clear();
                        mark_progress_dirty();
                    }
                    current_state = STATE_READER;
                    render_current_state();
                } else if (current_state == STATE_SETTINGS) {
                    switch ((SettingsItem)selected_settings_idx) {
                        case SETTINGS_FONT:
                            display_cycle_font();
                            page_history_clear(); // font, size and spacing change page breaks
                            break;
                        case SETTINGS_SIZE:
                            display_cycle_size();
                            page_history_clear();
                            break;
                        case SETTINGS_SPACING:
                            display_cycle_spacing();
                            page_history_clear();
                            break;
                        case SETTINGS_CONTRAST:
                            display_cycle_contrast();
                            break;
                        case SETTINGS_ORIENTATION:
                            display_set_flipped(!display_is_flipped());
                            buttons_set_swapped(display_is_flipped());
                            break;
                        case SETTINGS_REFRESH:
                            selected_refresh_menu_idx = 0;
                            current_state = STATE_REFRESH_SETTINGS;
                            break;
                        case SETTINGS_FIRMWARE_UPDATE:
                            flush_progress();
                            save_prefs();
                            display_draw_message("Firmware update", "Rebooting into the UF2 bootloader...", false);
                            reboot_to_bootloader();
                            break;
                        case SETTINGS_BACK:
                            current_state = settings_return_state;
                            break;
                        default:
                            break;
                    }
                    save_prefs();
                    render_current_state();
                } else if (current_state == STATE_REFRESH_SETTINGS) {
                    enum gdey037t03_refresh_profile prof = display_get_refresh_profile();
                    if (selected_refresh_menu_idx == 0) {
                        display_cycle_refresh_profile();
                        selected_refresh_menu_idx = 0;
                    } else if (prof == GDEY037T03_PROFILE_CUSTOM && selected_refresh_menu_idx == 1) {
                        display_cycle_pulse_interval();
                    } else if (prof == GDEY037T03_PROFILE_CUSTOM && selected_refresh_menu_idx == 2) {
                        display_cycle_pulse_count();
                    } else {
                        current_state = STATE_SETTINGS;
                    }
                    save_prefs();
                    render_current_state();
                } else if (current_state == STATE_TRANSFER) {
                    switch ((TransferItem)selected_transfer_idx) {
                        case TRANSFER_USB:
                            usb_storage_enter();
                            current_state = STATE_USB_STORAGE;
                            break;
                        case TRANSFER_BLUETOOTH:
                            current_state = STATE_BLE_UPLOAD;
                            ble_init(NULL, NULL);
                            ble_start_advertising();
                            break;
                        default:
                            current_state = STATE_MENU;
                            break;
                    }
                    render_current_state();
                } else if (current_state == STATE_LOCKSCREEN || current_state == STATE_STATS ||
                           current_state == STATE_BLE_UPLOAD || current_state == STATE_USB_STORAGE) {
                    if (current_state == STATE_USB_STORAGE) {
                        usb_storage_exit();
                    }
                    current_state = STATE_MENU;
                    render_current_state();
                }
                break;

            case BTN_EVENT_SELECT_LONG_PRESS:
                // Long press: back to the home carousel from anywhere below it; sleep from the top level
                if (current_state == STATE_READER || current_state == STATE_READER_MENU ||
                    current_state == STATE_SETTINGS || current_state == STATE_REFRESH_SETTINGS ||
                    current_state == STATE_BOOK_LIST || current_state == STATE_CHAPTER_LIST ||
                    current_state == STATE_TRANSFER) {
                    current_state = STATE_MENU;
                    render_current_state();
                } else {
                    if (current_state == STATE_USB_STORAGE) {
                        usb_storage_exit();
                    }
                    pre_sleep_state = current_state;
                    current_state = STATE_LOCKSCREEN;
                    render_current_state();
                }
                break;

            default:
                break;
        }

        if (progress_dirty && k_uptime_get_32() - progress_dirty_ms >= PROGRESS_SAVE_DELAY_MS) {
            flush_progress();
        }

        // Left the reader: bank the reading time and persist it with the stats
        if (reading_mark_ms != 0 && current_state != STATE_READER) {
            update_reading_time(false);
            flush_progress();
            save_prefs();
        }

        if (current_state == STATE_USB_STORAGE) {
            last_activity_time = k_uptime_get_32(); // no auto-sleep while the computer has the card
        } else if (k_uptime_get_32() - last_activity_time >= AUTO_SLEEP_MS) {
            pre_sleep_state = current_state;
            current_state = STATE_LOCKSCREEN;
            render_current_state();
        }
    }
    return 0;
}
