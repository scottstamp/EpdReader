#ifndef GRAYSCALE_FONT_H
#define GRAYSCALE_FONT_H

#include <Arduino.h>

struct GrayscaleGlyph {
    uint32_t bitmapOffset;     // Pointer into bitmaps array (offset in bits or bytes? Let's use bits/bytes)
    uint8_t  width;            // Bitmap width in pixels
    uint8_t  height;           // Bitmap height in pixels
    uint8_t  xAdvance;         // Distance to advance cursor (x axis)
    int8_t   xOffset;          // X offset from cursor position
    int8_t   yOffset;          // Y offset from cursor position
};

struct GrayscaleFont {
    const uint8_t *bitmap;     // Packed glyph bitmaps array (2 bits per pixel)
    const GrayscaleGlyph *glyph; // Glyph metadata array
    uint8_t first;             // First character ASCII code
    uint8_t last;              // Last character ASCII code
    uint8_t yAdvance;          // Line height/advance (y axis)
};

#endif // GRAYSCALE_FONT_H
