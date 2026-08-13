#include "font_extended.h"
#include <stddef.h>

/* Native Extended Bitmap Data for Common Unicode Characters (Latin Ext, Cyrillic, Greek) */
static const uint8_t extended_font_bitmaps[] = {
    // 0x00A0..0x00FF Latin Supplement Sample Glyphs
    0xFF, 0x81, 0x81, 0x81, 0xFF, // Box/accent
    0x38, 0x44, 0x92, 0xAA, 0x44, // Accented A
    0x7C, 0x12, 0x12, 0x12, 0x7C, // Accented E
    0x38, 0x10, 0x10, 0x10, 0x38, // Accented I
    0x38, 0x44, 0x44, 0x44, 0x38, // Accented O
    0x44, 0x44, 0x44, 0x44, 0x38, // Accented U
    0x7C, 0x40, 0x78, 0x40, 0x7C, // E-acute
    0x38, 0x04, 0x38, 0x40, 0x3C, // e-acute
    0x10, 0x28, 0x44, 0x7C, 0x44, // Alpha
    0x78, 0x44, 0x78, 0x44, 0x78, // Beta
    0x7C, 0x40, 0x40, 0x40, 0x40, // Gamma
    0x10, 0x28, 0x44, 0x44, 0x7C, // Delta
    0x38, 0x44, 0x38, 0x40, 0x3C, // Cyrillic a
    0x78, 0x44, 0x78, 0x44, 0x78, // Cyrillic б
    0x78, 0x44, 0x70, 0x44, 0x78, // Cyrillic в
    0x7C, 0x40, 0x40, 0x40, 0x40, // Cyrillic г
    0x10, 0x28, 0x28, 0x44, 0xFE  // Cyrillic д
};

static const GFXExtendedGlyph extended_glyphs[] = {
    { 0x00C0, { 0, 5, 5, 6, 0, -5 } }, // À
    { 0x00C1, { 5, 5, 5, 6, 0, -5 } }, // Á
    { 0x00C2, { 10, 5, 5, 6, 0, -5 } },// Â
    { 0x00C4, { 15, 5, 5, 6, 0, -5 } },// Ä
    { 0x00C7, { 20, 5, 5, 6, 0, -5 } },// Ç
    { 0x00C8, { 25, 5, 5, 6, 0, -5 } },// È
    { 0x00C9, { 30, 5, 5, 6, 0, -5 } },// É
    { 0x00CA, { 35, 5, 5, 6, 0, -5 } },// Ê
    { 0x00CB, { 40, 5, 5, 6, 0, -5 } },// Ë
    { 0x00E0, { 45, 5, 5, 6, 0, -5 } },// à
    { 0x00E1, { 50, 5, 5, 6, 0, -5 } },// á
    { 0x00E2, { 55, 5, 5, 6, 0, -5 } },// â
    { 0x00E4, { 60, 5, 5, 6, 0, -5 } },// ä
    { 0x00E7, { 65, 5, 5, 6, 0, -5 } },// ç
    { 0x00E8, { 70, 5, 5, 6, 0, -5 } },// è
    { 0x00E9, { 75, 5, 5, 6, 0, -5 } },// é
    { 0x00EA, { 80, 5, 5, 6, 0, -5 } },// ê
    { 0x00EB, { 85, 5, 5, 6, 0, -5 } },// ë
    // Greek
    { 0x03B1, { 40, 5, 5, 6, 0, -5 } },// α
    { 0x03B2, { 45, 5, 5, 6, 0, -5 } },// β
    { 0x03B3, { 50, 5, 5, 6, 0, -5 } },// γ
    { 0x03B4, { 55, 5, 5, 6, 0, -5 } },// δ
    // Cyrillic
    { 0x0430, { 60, 5, 5, 6, 0, -5 } },// а
    { 0x0431, { 65, 5, 5, 6, 0, -5 } },// б
    { 0x0432, { 70, 5, 5, 6, 0, -5 } },// в
    { 0x0433, { 75, 5, 5, 6, 0, -5 } },// г
    { 0x0434, { 80, 5, 5, 6, 0, -5 } } // д
};

static const uint16_t num_ext_glyphs = sizeof(extended_glyphs) / sizeof(extended_glyphs[0]);

const GFXglyph *font_get_glyph(uint32_t cp, const GFXfont *font, const uint8_t **out_bitmap) {
    if (!font) return NULL;

    if (cp >= font->first && cp <= font->last) {
        if (out_bitmap) *out_bitmap = font->bitmap;
        return &font->glyph[cp - font->first];
    }

    // Search extended Unicode glyph list for exact code point
    for (uint16_t i = 0; i < num_ext_glyphs; i++) {
        if (extended_glyphs[i].code_point == cp) {
            if (out_bitmap) *out_bitmap = extended_font_bitmaps;
            return &extended_glyphs[i].glyph;
        }
    }

    return NULL;
}
