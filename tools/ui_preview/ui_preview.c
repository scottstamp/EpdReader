// Host preview: renders firmware screens with the real display.c and dumps each one as a
// PBM into the directory given as argv[1]. Run through preview.py, which builds this and
// converts the dumps to PNG. Hardware-facing calls are stubbed below.
#include <stdio.h>
#include "display.h"
#include "storage.h"
#include "battery.h"
#include "epub.h"

const struct device stub_epd_device;
static const uint8_t *last_fb;
static int fb_count;

int display_write(const struct device *dev, uint16_t x, uint16_t y,
                  const struct display_buffer_descriptor *desc, const void *buf) {
    (void)dev; (void)x; (void)y; (void)desc;
    last_fb = buf;
    fb_count++;
    return 0;
}
void gdey037t03_request_full_refresh(const struct device *dev) { (void)dev; }
int gdey037t03_set_refresh_config(const struct device *dev, const struct gdey037t03_refresh_config *cfg) { (void)dev; (void)cfg; return 0; }
int gdey037t03_get_refresh_config(const struct device *dev, struct gdey037t03_refresh_config *cfg) {
    (void)dev; cfg->profile = GDEY037T03_PROFILE_NORMAL; cfg->pulse_interval = 2; cfg->pulse_count = 3; cfg->frame_rate = 0; return 0;
}
uint8_t battery_get_percentage(void) { return 85; }
const EpubBook *storage_get_epub(const char *n) { (void)n; return NULL; }
bool epub_extract_cover_image(const EpubBook *b, uint8_t *g, int w, int h) { (void)b; (void)g; (void)w; (void)h; return false; }

// Writes the logical 416x240 landscape view (flipped orientation, the default)
static void dump(const char *path) {
    FILE *f = fopen(path, "wb");
    fprintf(f, "P4\n%d %d\n", EPD_WIDTH, EPD_HEIGHT);
    for (int y = 0; y < EPD_HEIGHT; y++) {
        uint8_t row[(EPD_WIDTH + 7) / 8] = {0};
        for (int x = 0; x < EPD_WIDTH; x++) {
            int px = y, py = (EPD_PHYS_HEIGHT - 1) - x;
            int white = (last_fb[px / 8 + py * (EPD_PHYS_WIDTH / 8)] >> (7 - px % 8)) & 1;
            if (!white) row[x / 8] |= 0x80 >> (x % 8); // PBM: 1 = black
        }
        fwrite(row, 1, sizeof(row), f);
    }
    fclose(f);
}

int main(int argc, char **argv) {
    (void)argc;
    const CarouselItem items[] = {
        { &icon_continue_large, &icon_continue_small, "Continue", "He Who Fights with Monsters 1 - Shirtaloon - 42%" },
        { &icon_library_large, &icon_library_small, "Library", "Books on the SD card" },
        { &icon_settings_large, &icon_settings_small, "Settings", "Text, display, firmware" },
        { &icon_transfer_large, &icon_transfer_small, "Transfer", "USB drive or Bluetooth" },
        { &icon_stats_large, &icon_stats_small, "Stats", "12h 05m read in total" },
        { &icon_sleep_large, &icon_sleep_small, "Sleep", "Lock screen and power off" },
    };
    char path[512];
    for (int i = 0; i < 6; i++) {
        display_draw_carousel(items, 6, i);
        snprintf(path, sizeof(path), "%s/carousel_%d.pbm", argv[1], i);
        dump(path);
    }
    const char *settings[] = { "Font: Serif (Bookerly)", "Size: Medium (12pt)", "Spacing: Normal", "Contrast: Normal",
                               "Orientation: Flipped", "Refresh: Normal", "Firmware update (bootloader)", "[Back]" };
    display_draw_menu("Settings", settings, 8, 0);
    snprintf(path, sizeof(path), "%s/settings.pbm", argv[1]);
    dump(path);
    printf("rendered %d frames\n", fb_count);
    return 0;
}
