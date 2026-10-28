// Settings, refresh settings, and persisting them (with the reading stats) in NVS
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>
#include <hal/nrf_power.h>
#include <stdio.h>
#include "book.h"
#include "buttons.h"
#include "display.h"
#include "prefs.h"
#include "screens.h"

LOG_MODULE_REGISTER(settings_ui, LOG_LEVEL_INF);

// --- Persistence ---

void user_settings_save(void) {
    enum gdey037t03_refresh_profile profile;
    uint8_t interval, count, frame_rate;
    display_get_refresh_config(&profile, &interval, &count, &frame_rate);

    Prefs prefs = {
        .font = (uint8_t)display_get_font(),
        .size = (uint8_t)display_get_size(),
        .spacing = (uint8_t)display_get_spacing(),
        .contrast = (uint8_t)display_get_contrast(),
        .flipped = display_is_flipped() ? 1 : 0,
        .refresh_profile = (uint8_t)profile,
        .pulse_interval = interval,
        .pulse_count = count,
        .reading_seconds = reading_total_seconds(),
        .frame_rate = frame_rate,
    };
    prefs_save(&prefs);
}

void user_settings_load(void) {
    Prefs prefs;
    if (prefs_load(&prefs)) {
        if (prefs.font <= FONT_SD) display_set_font((FontType)prefs.font);
        if (prefs.size <= SIZE_MEDIUM) display_set_size((FontSize)prefs.size);
        if (prefs.spacing <= SPACING_RELAXED) display_set_spacing((LineSpacing)prefs.spacing);
        if (prefs.contrast <= CONTRAST_INVERTED) display_app_set_contrast((ContrastMode)prefs.contrast);
        display_set_flipped(prefs.flipped != 0);
        if (prefs.refresh_profile <= GDEY037T03_PROFILE_CUSTOM) {
            display_set_refresh_config((enum gdey037t03_refresh_profile)prefs.refresh_profile,
                                       prefs.pulse_interval, prefs.pulse_count, prefs.frame_rate);
        }
        reading_set_total_seconds(prefs.reading_seconds);
    }
    buttons_set_swapped(display_is_flipped());
}

// --- Settings ---

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

static int selected = 0;
// Settings opens from both the home carousel and the reader menu; Back returns there
static SystemState return_state = STATE_MENU;

static void settings_enter(SystemState from) {
    if (from != STATE_REFRESH_SETTINGS) {
        return_state = from;
        selected = 0;
    }
}

static void settings_render(void) {
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
    display_draw_menu("Settings", options, SETTINGS_COUNT, selected);
}

static void settings_step(int delta) {
    selected = wrap_index(selected, delta, SETTINGS_COUNT);
    app_redraw();
}

static void reboot_to_bootloader(void) {
    book_flush_progress();
    user_settings_save();
    display_draw_message("Firmware update", "Rebooting into the UF2 bootloader...", false);
    LOG_INF("Rebooting into Adafruit UF2 Bootloader...");
    nrf_power_gpregret_set(NRF_POWER, 0, 0x57);
    sys_reboot(SYS_REBOOT_WARM);
}

static void settings_select(void) {
    switch ((SettingsItem)selected) {
        case SETTINGS_FONT:
            display_cycle_font();
            book_reset_pagination(); // font, size and spacing change page breaks
            break;
        case SETTINGS_SIZE:
            display_cycle_size();
            book_reset_pagination();
            break;
        case SETTINGS_SPACING:
            display_cycle_spacing();
            book_reset_pagination();
            break;
        case SETTINGS_CONTRAST:
            display_cycle_contrast();
            break;
        case SETTINGS_ORIENTATION:
            display_set_flipped(!display_is_flipped());
            buttons_set_swapped(display_is_flipped());
            break;
        case SETTINGS_REFRESH:
            app_go(STATE_REFRESH_SETTINGS);
            return;
        case SETTINGS_FIRMWARE_UPDATE:
            reboot_to_bootloader();
            return;
        case SETTINGS_BACK:
            app_go(return_state);
            return;
        default:
            break;
    }
    user_settings_save();
    app_redraw();
}

const Screen screen_settings = {
    .enter = settings_enter,
    .render = settings_render,
    .step = settings_step,
    .select = settings_select,
};

// --- Refresh settings ---

typedef enum {
    REFRESH_PROFILE,
    REFRESH_INTERVAL,   // Custom profile only, like the two below
    REFRESH_COUNT,
    REFRESH_FRAME_RATE,
    REFRESH_BACK,
} RefreshItem;

static int refresh_selected = 0;

// Rows on screen for the current profile
static int refresh_items(RefreshItem items[5]) {
    int n = 0;
    items[n++] = REFRESH_PROFILE;
    if (display_get_refresh_profile() == GDEY037T03_PROFILE_CUSTOM) {
        items[n++] = REFRESH_INTERVAL;
        items[n++] = REFRESH_COUNT;
        items[n++] = REFRESH_FRAME_RATE;
    }
    items[n++] = REFRESH_BACK;
    return n;
}

static void refresh_enter(SystemState from) {
    (void)from;
    refresh_selected = 0;
}

static void refresh_render(void) {
    char prof_str[32], int_str[32], cnt_str[32], fr_str[32];
    snprintf(prof_str, sizeof(prof_str), "Profile: %s", display_get_refresh_profile_name());
    snprintf(int_str, sizeof(int_str), "Pulse Interval: %u", display_get_pulse_interval());
    snprintf(cnt_str, sizeof(cnt_str), "Pulse Count: %u", display_get_pulse_count());
    uint8_t frame_rate = display_get_frame_rate();
    if (frame_rate) {
        snprintf(fr_str, sizeof(fr_str), "Frame Rate: %u Hz", frame_rate);
    } else {
        snprintf(fr_str, sizeof(fr_str), "Frame Rate: Default");
    }

    const char *labels[] = {
        [REFRESH_PROFILE] = prof_str,
        [REFRESH_INTERVAL] = int_str,
        [REFRESH_COUNT] = cnt_str,
        [REFRESH_FRAME_RATE] = fr_str,
        [REFRESH_BACK] = "[Back]",
    };
    RefreshItem items[5];
    int n = refresh_items(items);
    const char *options[5];
    for (int i = 0; i < n; i++) options[i] = labels[items[i]];

    // Shorter waveforms turn pages faster but erase old text less fully
    const char *help;
    switch (display_get_refresh_profile()) {
        case GDEY037T03_PROFILE_NORMAL: help = "Normal: interval 2, 3 pulses, 80 Hz - clean"; break;
        case GDEY037T03_PROFILE_FAST:   help = "Fast: interval 2, 2 pulses, default rate - more ghosting"; break;
        case GDEY037T03_PROFILE_TURBO:  help = "Turbo: interval 1, 1 pulse, default rate - most ghosting"; break;
        default:                        help = "Custom: higher frame rate = shorter refresh"; break;
    }
    display_draw_menu_ext("Refresh Settings", help, options, n, refresh_selected);
}

static void refresh_step(int delta) {
    RefreshItem items[5];
    refresh_selected = wrap_index(refresh_selected, delta, refresh_items(items));
    app_redraw();
}

static void refresh_select(void) {
    RefreshItem items[5];
    refresh_items(items);
    switch (items[refresh_selected]) {
        case REFRESH_PROFILE:
            display_cycle_refresh_profile();
            refresh_selected = 0; // the rows below change with the profile
            break;
        case REFRESH_INTERVAL:   display_cycle_pulse_interval(); break;
        case REFRESH_COUNT:      display_cycle_pulse_count(); break;
        case REFRESH_FRAME_RATE: display_cycle_frame_rate(); break;
        case REFRESH_BACK:
            user_settings_save();
            app_go(STATE_SETTINGS);
            return;
    }
    user_settings_save();
    app_redraw();
}

const Screen screen_refresh_settings = {
    .enter = refresh_enter,
    .render = refresh_render,
    .step = refresh_step,
    .select = refresh_select,
};
