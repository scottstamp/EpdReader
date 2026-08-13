#include <zephyr/kernel.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usb_device.h>
#include <hal/nrf_power.h>
#include "config.h"
#include "display.h"
#include "storage.h"
#include "buttons.h"
#include "ble.h"
#include "battery.h"
#include <stdio.h>
#include <string.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

typedef enum {
    STATE_MENU,
    STATE_BOOK_LIST,
    STATE_READER,
    STATE_READER_MENU,
    STATE_FONT_SETTINGS,
    STATE_LOCKSCREEN,
    STATE_STATS,
    STATE_BLE_UPLOAD,
    STATE_USB_STORAGE
} SystemState;

static SystemState current_state = STATE_MENU;
static uint32_t last_activity_time = 0;
static char book_list[MAX_BOOKS][64];
static int book_count = 0;
static int selected_book_idx = 0;
static char active_book[64] = "";
static uint32_t current_page_offset = 0;
static uint32_t last_page_bytes_read = 300;
static uint32_t total_reading_seconds = 0;

static uint32_t session_start_ms = 0;

static const char *main_menu_options[] = {
    "Bootloader Mode",
    "Resume Reading",
    "Book List (SD)",
    "USB Transfer",
    "BLE Upload Mode",
    "Reading Stats",
    "Lock Screen"
};
static int main_menu_count = 7;
static int selected_menu_idx = 0;

static int selected_reader_menu_idx = 0;
static int selected_font_menu_idx = 0;

static SystemState pre_sleep_state = STATE_MENU;

static void format_session_time(uint32_t sec, char *out_buf, size_t buf_size) {
    uint32_t mins = sec / 60;
    uint32_t s = sec % 60;
    snprintf(out_buf, buf_size, "Session: %um %us", mins, s);
}

static void enter_deep_sleep(void) {
    LOG_INF("Entering System OFF deep sleep...");
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

static void render_current_state(void) {
    switch (current_state) {
        case STATE_MENU:
            display_draw_menu("Main Menu", main_menu_options, main_menu_count, selected_menu_idx);
            break;

        case STATE_BOOK_LIST: {
            const char *ptrs[MAX_BOOKS];
            for (int i = 0; i < book_count; i++) ptrs[i] = book_list[i];
            if (book_count == 0) {
                display_draw_message("Book List", "No books found on SD!\nAdd .txt/.epub to /SD/BOOKS", false);
            } else {
                display_draw_menu("Select Book", ptrs, book_count, selected_book_idx);
            }
            break;
        }

        case STATE_READER: {
            if (session_start_ms == 0) {
                session_start_ms = k_uptime_get_32();
            }

            char display_title[64];
            storage_get_book_display_title(active_book, display_title, sizeof(display_title));

            char page_buf[1024] = {0};
            uint32_t bytes_read = 0;

            bool ok = storage_read_book_page(active_book, current_page_offset, page_buf, sizeof(page_buf), &bytes_read);
            if (!ok || bytes_read == 0) {
                display_draw_message(display_title[0] ? display_title : "Reader Error", "Could not read book file!\nCheck SD card", true);
            } else {
                uint32_t file_size = storage_get_book_size(active_book);
                uint32_t progress_pct = (file_size > 0) ? ((current_page_offset * 100) / file_size) : 0;
                if (progress_pct > 100) progress_pct = 100;

                last_page_bytes_read = display_draw_reader_page(display_title, page_buf, progress_pct);
                if (last_page_bytes_read == 0) last_page_bytes_read = 300;
            }
            break;
        }

        case STATE_READER_MENU: {
            uint32_t file_size = storage_get_book_size(active_book);
            uint32_t progress_pct = (file_size > 0) ? ((current_page_offset * 100) / file_size) : 0;
            if (progress_pct > 100) progress_pct = 100;

            char display_title[64];
            storage_get_book_display_title(active_book, display_title, sizeof(display_title));

            char header_str[64];
            snprintf(header_str, sizeof(header_str), "%s (%u%%)", display_title[0] ? display_title : "Reader", progress_pct);

            char session_str[32];
            uint32_t session_sec = (session_start_ms > 0) ? ((k_uptime_get_32() - session_start_ms) / 1000) : 0;
            format_session_time(session_sec, session_str, sizeof(session_str));

            char flip_str[32];
            snprintf(flip_str, sizeof(flip_str), "Orientation: %s", display_is_flipped() ? "Flipped" : "Normal");

            const char *reader_options[] = {
                "Resume Reading",
                "Font Settings",
                flip_str,
                "Refresh Display",
                "Lock Screen",
                "Exit to Main Menu"
            };

            display_draw_menu_ext(header_str, session_str, reader_options, 6, selected_reader_menu_idx);
            break;
        }

        case STATE_FONT_SETTINGS: {
            char font_str[32];
            FontType f = display_get_font();
            const char *f_name = (f == FONT_SANS) ? "Sans (Ember)" : "Serif (Bookerly)";
            snprintf(font_str, sizeof(font_str), "Font: %s", f_name);

            char size_str[32];
            FontSize s = display_get_size();
            const char *s_name = (s == SIZE_SMALL) ? "Small (9pt)" : "Medium (12pt)";
            snprintf(size_str, sizeof(size_str), "Size: %s", s_name);

            char spacing_str[32];
            LineSpacing sp = display_get_spacing();
            const char *sp_name = (sp == SPACING_COMPACT) ? "Compact" : ((sp == SPACING_RELAXED) ? "Relaxed" : "Normal");
            snprintf(spacing_str, sizeof(spacing_str), "Spacing: %s", sp_name);

            char contrast_str[32];
            ContrastMode cm = display_get_contrast();
            snprintf(contrast_str, sizeof(contrast_str), "Contrast: %s", (cm == CONTRAST_INVERTED) ? "Inverted" : "Normal");

            const char *font_options[] = {
                font_str,
                size_str,
                spacing_str,
                contrast_str,
                "[Back]"
            };

            display_draw_menu("Font Settings", font_options, 5, selected_font_menu_idx);
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

            uint32_t file_size = active_book[0] ? storage_get_book_size(active_book) : 0;
            uint32_t progress_pct = (file_size > 0) ? ((current_page_offset * 100) / file_size) : 0;
            if (progress_pct > 100) progress_pct = 100;

            display_draw_lockscreen(active_book, title, author, progress_pct);

            // Save pre-sleep state to storage so wake-up restores exact view
            storage_write_sleep_state((int)pre_sleep_state, selected_menu_idx, active_book);

            // Enter deep sleep immediately after displaying lockscreen
            enter_deep_sleep();
            break;
        }

        case STATE_STATS: {
            char msg[64];
            snprintf(msg, sizeof(msg), "Total Reading Time:\n%u seconds", total_reading_seconds);
            display_draw_message("Reading Stats", msg, false);
            break;
        }

        case STATE_BLE_UPLOAD:
            display_draw_message("BLE Upload", "Advertising as:\nnRF EPD Reader", false);
            break;

        case STATE_USB_STORAGE:
            display_draw_message("USB Drive Mode", "USB Drive Active!\nPlug USB to add books", false);
            break;
    }
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

#if defined(CONFIG_USB_DEVICE_STACK)
    usb_enable(NULL);
#endif

    LOG_INF("Starting nRF e-Paper Book Reader (Zephyr RTOS)");

    display_init();
    storage_init();
    buttons_init();
    battery_init();

    storage_read_stats(&total_reading_seconds);

    int restored_state = -1;
    int restored_idx = 0;
    char restored_book[64] = {0};

    if (storage_read_sleep_state(&restored_state, &restored_idx, restored_book) && restored_state >= 0) {
        if (restored_book[0] != '\0') {
            snprintf(active_book, sizeof(active_book), "%s", restored_book);
            uint32_t saved_offset = 0;
            char temp_b[64];
            if (storage_read_progress(temp_b, &saved_offset)) {
                current_page_offset = saved_offset;
            }
        }
        if (restored_state == (int)STATE_READER || restored_state == (int)STATE_READER_MENU) {
            current_state = STATE_READER;
        } else if (restored_state == (int)STATE_BOOK_LIST) {
            current_state = STATE_BOOK_LIST;
            book_count = storage_list_sd_books(book_list, MAX_BOOKS);
        } else {
            current_state = STATE_MENU;
            selected_menu_idx = restored_idx;
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
        ButtonEvent evt = buttons_poll();
        if (evt != BTN_EVENT_NONE) {
            last_activity_time = k_uptime_get_32();
        }

        switch (evt) {
            case BTN_EVENT_PREV_CLICK:
                if (current_state == STATE_MENU) {
                    selected_menu_idx = (selected_menu_idx > 0) ? selected_menu_idx - 1 : main_menu_count - 1;
                    render_current_state();
                } else if (current_state == STATE_BOOK_LIST) {
                    if (book_count > 0) {
                        selected_book_idx = (selected_book_idx > 0) ? selected_book_idx - 1 : book_count - 1;
                        render_current_state();
                    }
                } else if (current_state == STATE_READER) {
                    if (current_page_offset >= last_page_bytes_read) {
                        current_page_offset -= last_page_bytes_read;
                    } else {
                        current_page_offset = 0;
                    }
                    storage_write_progress(active_book, current_page_offset);
                    render_current_state();
                } else if (current_state == STATE_READER_MENU) {
                    selected_reader_menu_idx = (selected_reader_menu_idx > 0) ? selected_reader_menu_idx - 1 : 5;
                    render_current_state();
                } else if (current_state == STATE_FONT_SETTINGS) {
                    selected_font_menu_idx = (selected_font_menu_idx > 0) ? selected_font_menu_idx - 1 : 4;
                    render_current_state();
                }
                break;

            case BTN_EVENT_NEXT_CLICK:
                if (current_state == STATE_MENU) {
                    selected_menu_idx = (selected_menu_idx + 1) % main_menu_count;
                    render_current_state();
                } else if (current_state == STATE_BOOK_LIST) {
                    if (book_count > 0) {
                        selected_book_idx = (selected_book_idx + 1) % book_count;
                        render_current_state();
                    }
                } else if (current_state == STATE_READER) {
                    current_page_offset += last_page_bytes_read;
                    storage_write_progress(active_book, current_page_offset);
                    render_current_state();
                } else if (current_state == STATE_READER_MENU) {
                    selected_reader_menu_idx = (selected_reader_menu_idx + 1) % 6;
                    render_current_state();
                } else if (current_state == STATE_FONT_SETTINGS) {
                    selected_font_menu_idx = (selected_font_menu_idx + 1) % 5;
                    render_current_state();
                }
                break;

            case BTN_EVENT_SELECT_CLICK:
                if (current_state == STATE_MENU) {
                    if (selected_menu_idx == 0) { // Bootloader Mode
                        display_draw_message("Bootloader Mode", "Rebooting...", false);
                        reboot_to_bootloader();
                    } else if (selected_menu_idx == 1) { // Resume
                        if (active_book[0] != '\0') {
                            current_state = STATE_READER;
                        } else {
                            display_draw_message("Resume", "No previous book found!", true);
                            k_msleep(1500);
                        }
                    } else if (selected_menu_idx == 2) { // Book List
                        book_count = storage_list_sd_books(book_list, MAX_BOOKS);
                        selected_book_idx = 0;
                        current_state = STATE_BOOK_LIST;
                    } else if (selected_menu_idx == 3) { // USB Transfer
                        current_state = STATE_USB_STORAGE;
                    } else if (selected_menu_idx == 4) { // BLE Upload
                        current_state = STATE_BLE_UPLOAD;
                        ble_init(NULL, NULL);
                        ble_start_advertising();
                    } else if (selected_menu_idx == 5) { // Stats
                        current_state = STATE_STATS;
                    } else if (selected_menu_idx == 6) { // Lockscreen
                        pre_sleep_state = STATE_MENU;
                        current_state = STATE_LOCKSCREEN;
                    }
                    render_current_state();
                } else if (current_state == STATE_BOOK_LIST) {
                    if (book_count > 0) {
                        snprintf(active_book, sizeof(active_book), "%s", book_list[selected_book_idx]);
                        current_page_offset = 0;
                        storage_write_progress(active_book, current_page_offset);
                        current_state = STATE_READER;
                        render_current_state();
                    }
                } else if (current_state == STATE_READER) {
                    selected_reader_menu_idx = 0;
                    current_state = STATE_READER_MENU;
                    render_current_state();
                } else if (current_state == STATE_READER_MENU) {
                    if (selected_reader_menu_idx == 0) { // Resume
                        current_state = STATE_READER;
                    } else if (selected_reader_menu_idx == 1) { // Font Settings
                        selected_font_menu_idx = 0;
                        current_state = STATE_FONT_SETTINGS;
                    } else if (selected_reader_menu_idx == 2) { // Orientation
                        display_set_flipped(!display_is_flipped());
                    } else if (selected_reader_menu_idx == 3) { // Refresh
                        display_update(true);
                        current_state = STATE_READER;
                    } else if (selected_reader_menu_idx == 4) { // Lockscreen
                        pre_sleep_state = STATE_READER;
                        current_state = STATE_LOCKSCREEN;
                    } else if (selected_reader_menu_idx == 5) { // Exit to main menu
                        current_state = STATE_MENU;
                    }
                    render_current_state();
                } else if (current_state == STATE_FONT_SETTINGS) {
                    if (selected_font_menu_idx == 0) {
                        display_cycle_font();
                    } else if (selected_font_menu_idx == 1) {
                        display_cycle_size();
                    } else if (selected_font_menu_idx == 2) {
                        display_cycle_spacing();
                    } else if (selected_font_menu_idx == 3) {
                        display_cycle_contrast();
                    } else if (selected_font_menu_idx == 4) {
                        current_state = STATE_READER_MENU;
                    }
                    render_current_state();
                } else if (current_state == STATE_LOCKSCREEN || current_state == STATE_STATS ||
                           current_state == STATE_BLE_UPLOAD || current_state == STATE_USB_STORAGE) {
                    current_state = STATE_MENU;
                    render_current_state();
                }
                break;

            case BTN_EVENT_SELECT_DOUBLE_CLICK:
                reboot_to_bootloader();
                break;

            case BTN_EVENT_SELECT_LONG_PRESS:
                if (current_state == STATE_READER || current_state == STATE_READER_MENU || current_state == STATE_FONT_SETTINGS) {
                    current_state = STATE_MENU;
                    render_current_state();
                } else {
                    pre_sleep_state = current_state;
                    current_state = STATE_LOCKSCREEN;
                    render_current_state();
                }
                break;

            default:
                break;
        }

        /* Keep activity timer fresh while in USB Mass Storage mode */
        if (current_state == STATE_USB_STORAGE) {
            last_activity_time = k_uptime_get_32();
        }

        if (current_state != STATE_USB_STORAGE &&
            (k_uptime_get_32() - last_activity_time > AUTO_SLEEP_MS)) {
            pre_sleep_state = current_state;
            current_state = STATE_LOCKSCREEN;
            render_current_state();
        }

        k_msleep(20);
    }
    return 0;
}
