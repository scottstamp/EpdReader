#ifndef DITHER_H
#define DITHER_H

// Grayscale -> 1-bit conversion for cover images, a row at a time, so a cover never needs a
// full 8-bit frame in RAM. Pure C (no Zephyr) so it can be tested on the host.
//
// 1. Collect a histogram of every pixel (dither_hist_row), e.g. in a first decode pass.
// 2. dither_build_lut(): stretch the image's own 2nd-98th percentile range to black-white
//    and darken midtones, so pale cover art and white titles survive 1-bit output.
//    Images that are already black and white just get thresholded.
// 3. dither_row() on each row in order: Floyd-Steinberg, packed MSB first, 1 = white.

#include <stdint.h>

typedef struct {
    int width;
    int y;
    const uint8_t *lut;
    int16_t *err;   // 2 * width, error in 1/16 units for this row and the next
} Ditherer;

void dither_hist_row(uint32_t hist[256], const uint8_t *row, int width);
void dither_build_lut(const uint32_t hist[256], uint8_t lut[256]);

void dither_start(Ditherer *d, int width, int16_t *err, const uint8_t *lut);
// bits receives (width + 7) / 8 bytes
void dither_row(Ditherer *d, const uint8_t *row, uint8_t *bits);

#endif // DITHER_H
