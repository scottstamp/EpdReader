#include "font_extended.h"
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stddef.h>

LOG_MODULE_REGISTER(font_sd, LOG_LEVEL_INF);

#define MAX_SD_GLYPH_INDEX 512

static struct fs_file_t s_font_file;
static bool s_sd_font_loaded = false;
static SdFontHeader s_font_hdr;
static SdGlyphIndexEntry s_glyph_index[MAX_SD_GLYPH_INDEX];
static uint16_t s_glyph_count = 0;

static GFXglyph s_cached_gfx_glyph;
static uint8_t s_cached_bitmap[128];

bool font_sd_load(const char *font_path) {
    if (!font_path || font_path[0] == '\0') return false;

    if (s_sd_font_loaded) {
        font_sd_unload();
    }

    fs_file_t_init(&s_font_file);
    int res = fs_open(&s_font_file, font_path, FS_O_READ);
    if (res != 0) {
        LOG_WRN("Failed to open SD font file: %s (err %d)", font_path, res);
        return false;
    }

    ssize_t r = fs_read(&s_font_file, &s_font_hdr, sizeof(SdFontHeader));
    if (r < sizeof(SdFontHeader) || memcmp(s_font_hdr.magic, "SDFN", 4) != 0) {
        LOG_ERR("Invalid SD font header/magic in file: %s", font_path);
        fs_close(&s_font_file);
        return false;
    }

    s_glyph_count = s_font_hdr.glyph_count;
    if (s_glyph_count > MAX_SD_GLYPH_INDEX) {
        s_glyph_count = MAX_SD_GLYPH_INDEX;
    }

    size_t idx_bytes = s_glyph_count * sizeof(SdGlyphIndexEntry);
    ssize_t ir = fs_read(&s_font_file, s_glyph_index, idx_bytes);
    if (ir < (ssize_t)idx_bytes) {
        LOG_ERR("Failed to read glyph index table from SD font: %s", font_path);
        fs_close(&s_font_file);
        return false;
    }

    s_sd_font_loaded = true;
    LOG_INF("Loaded SD font '%s': %u glyphs, size %upt, styles 0x%02X",
            font_path, s_glyph_count, s_font_hdr.font_size, s_font_hdr.style_flags);
    return true;
}

void font_sd_unload(void) {
    if (s_sd_font_loaded) {
        fs_close(&s_font_file);
        s_sd_font_loaded = false;
        s_glyph_count = 0;
        LOG_INF("Unloaded SD font");
    }
}

bool font_sd_is_active(void) {
    return s_sd_font_loaded;
}

static const GFXglyph *font_sd_lookup_glyph(uint32_t cp, uint8_t style_id, const uint8_t **out_bitmap) {
    if (!s_sd_font_loaded || s_glyph_count == 0) return NULL;

    uint32_t target_key = (cp & 0x3FFFFFFF) | (((uint32_t)style_id & 0x03) << 30);
    uint32_t fallback_key = (cp & 0x3FFFFFFF); // Style 0 (Regular)

    int match_idx = -1;

    // Binary search index table
    int low = 0, high = s_glyph_count - 1;
    while (low <= high) {
        int mid = (low + high) / 2;
        uint32_t key = s_glyph_index[mid].cp_style_encoded;
        if (key == target_key) {
            match_idx = mid;
            break;
        } else if (key < target_key) {
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }

    // Fallback to Regular style if target style not found
    if (match_idx < 0 && style_id != 0) {
        low = 0; high = s_glyph_count - 1;
        while (low <= high) {
            int mid = (low + high) / 2;
            uint32_t key = s_glyph_index[mid].cp_style_encoded;
            if (key == fallback_key) {
                match_idx = mid;
                break;
            } else if (key < fallback_key) {
                low = mid + 1;
            } else {
                high = mid - 1;
            }
        }
    }

    if (match_idx < 0) return NULL;

    uint32_t offset = s_glyph_index[match_idx].file_offset;
    fs_seek(&s_font_file, offset, FS_SEEK_SET);

    SdGlyphRecordHeader rec_hdr;
    if (fs_read(&s_font_file, &rec_hdr, sizeof(SdGlyphRecordHeader)) < sizeof(SdGlyphRecordHeader)) {
        return NULL;
    }

    uint16_t bm_len = rec_hdr.bitmap_len;
    if (bm_len > sizeof(s_cached_bitmap)) {
        bm_len = sizeof(s_cached_bitmap);
    }

    fs_read(&s_font_file, s_cached_bitmap, bm_len);

    s_cached_gfx_glyph.bitmapOffset = 0;
    s_cached_gfx_glyph.width        = rec_hdr.width;
    s_cached_gfx_glyph.height       = rec_hdr.height;
    s_cached_gfx_glyph.xAdvance     = rec_hdr.xAdvance;
    s_cached_gfx_glyph.xOffset      = rec_hdr.xOffset;
    s_cached_gfx_glyph.yOffset      = rec_hdr.yOffset;

    if (out_bitmap) {
        *out_bitmap = s_cached_bitmap;
    }
    return &s_cached_gfx_glyph;
}

const GFXglyph *font_get_glyph_styled(uint32_t cp, bool is_bold, bool is_italic, const GFXfont *font, const uint8_t **out_bitmap) {
    uint8_t style_id = 0;
    if (is_bold && is_italic) style_id = 3;
    else if (is_italic) style_id = 2;
    else if (is_bold) style_id = 1;

    if (s_sd_font_loaded) {
        const GFXglyph *g = font_sd_lookup_glyph(cp, style_id, out_bitmap);
        if (g) return g;
    }

    if (font && cp >= font->first && cp <= font->last) {
        if (out_bitmap) *out_bitmap = font->bitmap;
        return &font->glyph[cp - font->first];
    }

    return NULL;
}

const GFXglyph *font_get_glyph(uint32_t cp, const GFXfont *font, const uint8_t **out_bitmap) {
    return font_get_glyph_styled(cp, false, false, font, out_bitmap);
}
