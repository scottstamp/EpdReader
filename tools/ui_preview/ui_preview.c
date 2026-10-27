// Host preview: renders firmware screens with the real display.c and dumps each one as a
// PBM into the directory given as argv[1]. Run through preview.py, which builds this and
// converts the dumps to PNG. Hardware-facing calls are stubbed below.
#include <stdio.h>
#include <stdlib.h>
#include "display.h"
#include "storage.h"
#include "battery.h"
#include "epub.h"
#include "font_extended.h"

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
// The lock screen gets a cover when UI_PREVIEW_COVER names a raw 8-bit grayscale file of the
// requested size (e.g. a .cov cache file from the SD card)
static EpubBook preview_book;
const EpubBook *storage_get_epub(const char *n) { (void)n; return getenv("UI_PREVIEW_COVER") ? &preview_book : NULL; }
bool epub_extract_cover_image(const EpubBook *b, uint8_t *g, int w, int h) {
    (void)b;
    const char *path = getenv("UI_PREVIEW_COVER");
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f) return false;
    size_t got = fread(g, 1, (size_t)w * h, f);
    fclose(f);
    return got == (size_t)w * h;
}

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

// Accented sample text with the reader's inline style codes, a soft hyphen, a ligature
// and a word too long for one line
static const char reader_sample[] =
    "\x02" "Chapitre premier" "\x03" "\n\n"
    "« Ça va ? » demanda Zoë en entrant dans le café. “I’d rather not,” she said — and then, "
    "after a pause… ‘Fine.’ The 1914–1918 war; pages 3–7.\n\n"
    "Crème brûlée costs 5 € (or £4); naïve façade, señor Muñoz, Ångström, Øresund, smörgåsbord, "
    "Łódź, Dvořák, œuvre, Straße, Æsir.\n\n"
    "\x0E" "Italic: élève, déjà vu, piñata — “quoted” text." "\x0F" " "
    "\x15" "Underlined: résumé." "\x16" " " "\x02" "Bold: Ñandú über alles" "\x03" ".\n\n"
    "Soft hy\xC2\xAD" "phen, \xEF\xAC\x81" "ne ligature, zero\xE2\x80\x8Bwidth. "
    "PneumonoultramicroscopicsilicovolcanoconiosisàèìòùáéíóúâêîôûäëïöüçñÀÈÌÒÙÁÉÍÓÚ then more text to wrap.\n\n"
    "\x0E" "Une phrase en italique qui continue sur la page suivante, avec des accents : "
    "à, â, ç, é, è, ê, ë, î, ï, ô, ù, û, ü, ÿ, œ. "
    "Encore une phrase longue pour remplir la page et vérifier la pagination — “guillemets” … "
    "Encore une phrase longue pour remplir la page et vérifier la pagination — “guillemets” … "
    "Encore une phrase longue pour remplir la page et vérifier la pagination — “guillemets” … "
    "Encore une phrase longue pour remplir la page et vérifier la pagination — “guillemets” …" "\x0F" "\n\n"
    "Último párrafo: ¿Qué tal? ¡Muy bien! Grüße aus Köln. Señora Peña’s niño.\n";

static uint8_t scan_styles(const char *s, uint32_t len, uint8_t style) {
    for (uint32_t i = 0; i < len; i++) {
        switch (s[i]) {
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

static bool is_cont(char c) { return ((uint8_t)c & 0xC0) == 0x80; }

// Pages through the sample with the current reader font, checking that measuring and drawing
// agree and that page breaks never split a UTF-8 sequence. Returns the number of failures.
static int render_reader_pages(const char *dir, const char *font_name) {
    int failures = 0;
    const uint32_t len = sizeof(reader_sample) - 1;
    uint32_t offset = 0;
    uint8_t style = 0;
    char path[512];

    for (int page = 0; offset < len && page < 20; page++) {
        const char *text = reader_sample + offset;
        uint32_t measured = display_measure_reader_page(text);
        uint32_t drawn = display_draw_reader_page("Les Misérables — Tome I", text, offset * 100 / len, style);
        if (measured != drawn) {
            printf("FAIL %s page %d: measured %u != drawn %u\n", font_name, page, measured, drawn);
            failures++;
        }
        if (drawn == 0 || (offset + drawn < len && is_cont(reader_sample[offset + drawn]))) {
            printf("FAIL %s page %d: bad page break at %u\n", font_name, page, offset + drawn);
            failures++;
            break;
        }
        if (page < 2) {
            snprintf(path, sizeof(path), "%s/reader_%s_%d.pbm", dir, font_name, page + 1);
            dump(path);
        }
        style = scan_styles(text, drawn, style);
        offset += drawn;
    }

    // Truncated page buffers (as storage reads them) must not end a page mid-character
    static char buf[sizeof(reader_sample)];
    for (uint32_t cut = 1; cut <= len; cut++) {
        memcpy(buf, reader_sample, cut);
        buf[cut] = '\0';
        uint32_t c = display_measure_reader_page(buf);
        if (c > cut || (c < len && is_cont(reader_sample[c]))) {
            printf("FAIL %s: buffer of %u bytes measured to %u\n", font_name, cut, c);
            failures++;
            break;
        }
    }
    return failures;
}

// Pages through a whole converted book with 1 KB windows like main.c reads them, checking the
// same invariants, and dumps the page at 1/3 of the book. Returns the number of failures.
#define BOOK_PAGE_BUF_SIZE 1024
static int paginate_book(const char *dir, const char *txt_path, int book_idx, const char *font_name) {
    FILE *f = fopen(txt_path, "rb");
    if (!f) {
        printf("FAIL: cannot open %s\n", txt_path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    uint32_t len = (uint32_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    char *book = malloc(len + 1);
    len = (uint32_t)fread(book, 1, len, f);
    book[len] = '\0';
    fclose(f);

    int failures = 0, pages = 0;
    bool dumped = false;
    uint32_t offset = 0;
    uint8_t style = 0;
    char buf[BOOK_PAGE_BUF_SIZE];
    while (offset < len) {
        uint32_t got = MIN(len - offset, BOOK_PAGE_BUF_SIZE - 1);
        memcpy(buf, book + offset, got);
        buf[got] = '\0';
        uint32_t measured = display_measure_reader_page(buf);
        uint32_t drawn = display_draw_reader_page("Book", buf, offset * 100 / len, style);
        if (measured != drawn || drawn == 0 || drawn > got ||
            (offset + drawn < len && is_cont(book[offset + drawn]))) {
            printf("FAIL %s %s at %u: measured %u drawn %u\n", font_name, txt_path, offset, measured, drawn);
            failures++;
            break;
        }
        if (!dumped && offset >= len / 3) {
            char path[512];
            snprintf(path, sizeof(path), "%s/book%d_%s.pbm", dir, book_idx, font_name);
            dump(path);
            dumped = true;
        }
        style = scan_styles(buf, drawn, style);
        offset += drawn;
        pages++;
    }
    printf("%s %s: %d pages\n", font_name, txt_path, pages);
    free(book);
    return failures;
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

    // Menus and headers use the built-in fonts: accents fall back to ASCII
    const char *accented_menu[] = { "Les Misérables — Victor Hugo", "Cien años de soledad", "Über “Quotes” … 5 €",
                                    "[Back]" };
    display_draw_menu("Bibliothèque", accented_menu, 4, 1);
    snprintf(path, sizeof(path), "%s/menu_accents.pbm", argv[1]);
    dump(path);

    display_draw_lockscreen("book.epub", "The Martian", "Andy Weir", "Chapter 7", 42);
    snprintf(path, sizeof(path), "%s/lockscreen.pbm", argv[1]);
    dump(path);

    // Reader pages: the SD font (fonts/Arial.fnt via the stdio fs stubs) and the built-in fonts
    int failures = 0;
    if (!font_sd_load("/SD:/fonts/Arial.fnt")) {
        printf("FAIL: could not load fonts/Arial.fnt (set UI_PREVIEW_SD_ROOT to the repo root)\n");
        failures++;
    }
    static const struct { FontType font; FontSize size; const char *name; } reader_fonts[] = {
        { FONT_SD, SIZE_MEDIUM, "sd" },
        { FONT_SERIF, SIZE_MEDIUM, "serif" },
        { FONT_SANS, SIZE_SMALL, "sans_small" },
    };
    for (size_t f = 0; f < sizeof(reader_fonts) / sizeof(reader_fonts[0]); f++) {
        display_set_font(reader_fonts[f].font);
        display_set_size(reader_fonts[f].size);
        failures += render_reader_pages(argv[1], reader_fonts[f].name);
        for (int b = 2; b < argc; b++) {
            failures += paginate_book(argv[1], argv[b], b - 1, reader_fonts[f].name);
        }
    }
    display_set_font(FONT_SERIF);
    display_set_size(SIZE_MEDIUM);

    printf("rendered %d frames, %d check failures\n", fb_count, failures);
    return failures ? 1 : 0;
}
