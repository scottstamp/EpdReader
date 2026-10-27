#ifndef FONT_EXTENDED_H
#define FONT_EXTENDED_H

#include "gfxfont.h"
#include <stdint.h>
#include <stdbool.h>

#define SD_FONT_MAGIC 0x4E464453 // "SDFN"

typedef struct __attribute__((packed)) {
    char     magic[4];       // "SDFN"
    uint16_t version;        // 1
    uint16_t style_flags;    // bit0:Reg, bit1:Bold, bit2:Italic, bit3:BoldItalic
    uint16_t font_size;      // Point size
    int16_t  ascent;
    int16_t  descent;
    int16_t  line_height;
    uint16_t glyph_count;
    uint8_t  reserved[14];
} SdFontHeader;

typedef struct __attribute__((packed)) {
    uint32_t cp_style_encoded; // Top 2 bits = style_id, bottom 30 bits = codepoint
    uint32_t file_offset;      // File offset to glyph record
} SdGlyphIndexEntry;

typedef struct __attribute__((packed)) {
    uint8_t  style_id;     // 0=Reg, 1=Bold, 2=Italic, 3=BoldItalic
    uint8_t  width;
    uint8_t  height;
    int8_t   xOffset;
    int8_t   yOffset;
    uint8_t  xAdvance;
    uint16_t bitmap_len;
} SdGlyphRecordHeader;

// SD Font API: the font is copied into RAM by font_sd_load(), the file is not kept open
bool font_sd_load(const char *font_path);
void font_sd_unload(void);
bool font_sd_is_active(void);
int font_sd_line_height(void);

// Pass font == NULL to look up the loaded SD font, otherwise the built-in font is used.
// *has_style is set when the glyph already carries the requested bold/italic style
// (SD fonts only), so the caller should not synthesize it.
const GFXglyph *font_get_glyph(uint32_t cp, const GFXfont *font, const uint8_t **out_bitmap);
const GFXglyph *font_get_glyph_styled(uint32_t cp, bool is_bold, bool is_italic, const GFXfont *font,
                                      const uint8_t **out_bitmap, bool *has_style);

#endif // FONT_EXTENDED_H
