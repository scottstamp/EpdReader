#include "font_extended.h"
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stddef.h>

LOG_MODULE_REGISTER(font_sd, LOG_LEVEL_INF);

// RAM reserved for the SD font. Glyph entries grow up from the start and bitmaps
// grow down from the end; styles that don't fit completely are dropped and fall
// back to faux bold/italic. A 14px font needs ~9 KB per style.
#define SD_FONT_POOL_SIZE (24 * 1024)
BUILD_ASSERT(SD_FONT_POOL_SIZE <= UINT16_MAX, "bitmapOffset is 16-bit");

#define INDEX_CHUNK 32

typedef struct {
    uint32_t key;      // Top 2 bits = style_id, bottom 30 bits = codepoint
    GFXglyph glyph;    // bitmapOffset is relative to s_pool
} SdGlyph;

static uint8_t __aligned(4) s_pool[SD_FONT_POOL_SIZE];
static SdGlyph *const s_glyphs = (SdGlyph *)s_pool;
static uint16_t s_glyph_count = 0;
static bool s_sd_font_loaded = false;
static SdFontHeader s_font_hdr;

static uint32_t make_key(uint32_t cp, uint8_t style_id) {
    return (cp & 0x3FFFFFFF) | (((uint32_t)style_id & 0x03) << 30);
}

bool font_sd_load(const char *font_path) {
    if (!font_path || font_path[0] == '\0') return false;

    font_sd_unload();

    struct fs_file_t file;
    fs_file_t_init(&file);
    int res = fs_open(&file, font_path, FS_O_READ);
    if (res != 0) {
        LOG_WRN("Failed to open SD font file: %s (err %d)", font_path, res);
        return false;
    }

    uint32_t start_ms = k_uptime_get_32();
    ssize_t r = fs_read(&file, &s_font_hdr, sizeof(SdFontHeader));
    if (r < (ssize_t)sizeof(SdFontHeader) || memcmp(s_font_hdr.magic, "SDFN", 4) != 0) {
        LOG_ERR("Invalid SD font header/magic in file: %s", font_path);
        fs_close(&file);
        return false;
    }

    uint16_t count = 0;
    size_t bitmap_low = SD_FONT_POOL_SIZE;  // bitmaps occupy [bitmap_low, POOL_SIZE)
    int cur_style = -1;
    uint16_t style_start_count = 0;
    size_t style_start_low = bitmap_low;
    int skip_style = -1;
    uint8_t loaded_styles = 0;
    bool io_error = false;

    for (uint16_t base = 0; base < s_font_hdr.glyph_count && !io_error; base += INDEX_CHUNK) {
        SdGlyphIndexEntry idx[INDEX_CHUNK];
        uint16_t n = MIN(INDEX_CHUNK, s_font_hdr.glyph_count - base);
        fs_seek(&file, sizeof(SdFontHeader) + (off_t)base * sizeof(SdGlyphIndexEntry), FS_SEEK_SET);
        if (fs_read(&file, idx, n * sizeof(SdGlyphIndexEntry)) != (ssize_t)(n * sizeof(SdGlyphIndexEntry))) {
            io_error = true;
            break;
        }

        for (uint16_t i = 0; i < n; i++) {
            // The index is sorted by key, so each style is a contiguous run
            int style = (int)(idx[i].cp_style_encoded >> 30);
            if (style != cur_style) {
                if (cur_style >= 0 && cur_style != skip_style) loaded_styles |= BIT(cur_style);
                cur_style = style;
                style_start_count = count;
                style_start_low = bitmap_low;
            }
            if (style == skip_style) continue;

            SdGlyphRecordHeader rec;
            fs_seek(&file, idx[i].file_offset, FS_SEEK_SET);
            if (fs_read(&file, &rec, sizeof(rec)) != (ssize_t)sizeof(rec)) {
                io_error = true;
                break;
            }

            size_t entries_end = (size_t)(count + 1) * sizeof(SdGlyph);
            if (entries_end + rec.bitmap_len > bitmap_low) {
                // Style doesn't fit: drop the partial style entirely
                count = style_start_count;
                bitmap_low = style_start_low;
                skip_style = style;
                continue;
            }

            bitmap_low -= rec.bitmap_len;
            if (fs_read(&file, &s_pool[bitmap_low], rec.bitmap_len) != (ssize_t)rec.bitmap_len) {
                io_error = true;
                break;
            }

            SdGlyph *g = &s_glyphs[count++];
            g->key = idx[i].cp_style_encoded;
            g->glyph.bitmapOffset = (uint16_t)bitmap_low;
            g->glyph.width = rec.width;
            g->glyph.height = rec.height;
            g->glyph.xAdvance = rec.xAdvance;
            g->glyph.xOffset = rec.xOffset;
            g->glyph.yOffset = rec.yOffset;
        }
    }
    if (cur_style >= 0 && cur_style != skip_style) loaded_styles |= BIT(cur_style);
    fs_close(&file);

    if (io_error || count == 0 || !(loaded_styles & BIT(0))) {
        LOG_ERR("Failed to load SD font: %s (io_error %d, %u glyphs)", font_path, io_error, count);
        return false;
    }

    s_glyph_count = count;
    s_sd_font_loaded = true;
    LOG_INF("Loaded SD font '%s' into RAM: %u/%u glyphs, styles 0x%X, %u/%u bytes, %u ms",
            font_path, count, s_font_hdr.glyph_count, loaded_styles,
            (unsigned)(count * sizeof(SdGlyph) + (SD_FONT_POOL_SIZE - bitmap_low)), SD_FONT_POOL_SIZE,
            k_uptime_get_32() - start_ms);
    return true;
}

void font_sd_unload(void) {
    if (s_sd_font_loaded) {
        s_sd_font_loaded = false;
        s_glyph_count = 0;
        LOG_INF("Unloaded SD font");
    }
}

bool font_sd_is_active(void) {
    return s_sd_font_loaded;
}

int font_sd_line_height(void) {
    return s_sd_font_loaded ? s_font_hdr.line_height : 0;
}

static const SdGlyph *font_sd_find(uint32_t key) {
    int low = 0, high = (int)s_glyph_count - 1;
    while (low <= high) {
        int mid = (low + high) / 2;
        uint32_t k = s_glyphs[mid].key;
        if (k == key) return &s_glyphs[mid];
        if (k < key) low = mid + 1;
        else high = mid - 1;
    }
    return NULL;
}

const GFXglyph *font_get_glyph_styled(uint32_t cp, bool is_bold, bool is_italic, const GFXfont *font,
                                      const uint8_t **out_bitmap, bool *has_style) {
    uint8_t style_id = 0;
    if (is_bold && is_italic) style_id = 3;
    else if (is_italic) style_id = 2;
    else if (is_bold) style_id = 1;

    if (has_style) *has_style = false;

    if (font == NULL) {
        if (!s_sd_font_loaded) return NULL;

        const SdGlyph *g = font_sd_find(make_key(cp, style_id));
        if (g) {
            if (has_style) *has_style = true;
        } else if (style_id != 0) {
            // Fall back to Regular; the caller synthesizes the style
            g = font_sd_find(make_key(cp, 0));
        }
        if (!g) return NULL;
        if (out_bitmap) *out_bitmap = s_pool;
        return &g->glyph;
    }

    if (cp >= font->first && cp <= font->last) {
        if (out_bitmap) *out_bitmap = font->bitmap;
        return &font->glyph[cp - font->first];
    }

    return NULL;
}

const GFXglyph *font_get_glyph(uint32_t cp, const GFXfont *font, const uint8_t **out_bitmap) {
    return font_get_glyph_styled(cp, false, false, font, out_bitmap, NULL);
}
