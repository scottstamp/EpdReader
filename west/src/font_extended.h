#ifndef FONT_EXTENDED_H
#define FONT_EXTENDED_H

#include "gfxfont.h"
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint32_t code_point;
    GFXglyph glyph;
} GFXExtendedGlyph;

typedef struct {
    const uint8_t *bitmap;
    const GFXExtendedGlyph *glyphs;
    uint16_t count;
} GFXExtendedFont;

const GFXglyph *font_get_glyph(uint32_t cp, const GFXfont *font, const uint8_t **out_bitmap);

#endif // FONT_EXTENDED_H
