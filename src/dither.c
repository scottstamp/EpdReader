#include "dither.h"

#include <string.h>

void dither_hist_row(uint32_t hist[256], const uint8_t *row, int width) {
    for (int x = 0; x < width; x++) {
        hist[row[x]]++;
    }
}

void dither_build_lut(const uint32_t hist[256], uint8_t lut[256]) {
    uint32_t total = 0, mid = 0;
    for (int v = 0; v < 256; v++) {
        total += hist[v];
        if (v > 10 && v < 245) mid += hist[v];
    }

    if (mid == 0) {
        // Already black and white (line art, a pre-dithered image): plain threshold
        for (int v = 0; v < 256; v++) lut[v] = (v < 128) ? 0 : 255;
        return;
    }

    uint32_t clip = total / 50;
    int lo = 0, hi = 255;
    uint32_t acc = 0;
    for (int v = 0; v < 256; v++) {
        acc += hist[v];
        if (acc > clip) { lo = v; break; }
    }
    acc = 0;
    for (int v = 255; v >= 0; v--) {
        acc += hist[v];
        if (acc > clip) { hi = v; break; }
    }
    int span = (hi - lo > 1) ? hi - lo : 1;

    for (int v = 0; v < 256; v++) {
        int s = (v - lo) * 255 / span;
        s = (s < 0) ? 0 : (s > 255 ? 255 : s);
        // Halfway between linear and squared: roughly a 1.4 gamma
        lut[v] = (uint8_t)((s * 255 + s * s) / 510);
    }
}

void dither_start(Ditherer *d, int width, int16_t *err, const uint8_t *lut) {
    d->width = width;
    d->y = 0;
    d->lut = lut;
    d->err = err;
    memset(err, 0, 2 * (size_t)width * sizeof(int16_t));
}

void dither_row(Ditherer *d, const uint8_t *row, uint8_t *bits) {
    // Floyd-Steinberg keeps all of the quantization error (Atkinson dropped a quarter of it,
    // which washed out midtones)
    int16_t *cur = d->err + (d->y & 1) * d->width;
    int16_t *next = d->err + ((d->y + 1) & 1) * d->width;
    memset(next, 0, (size_t)d->width * sizeof(int16_t));
    memset(bits, 0, ((size_t)d->width + 7) / 8);

    for (int x = 0; x < d->width; x++) {
        int val = d->lut[row[x]] + cur[x] / 16;
        val = (val < 0) ? 0 : (val > 255 ? 255 : val);
        int out = (val >= 128) ? 255 : 0;
        int err = val - out;
        if (out) bits[x / 8] |= (uint8_t)(0x80 >> (x % 8));

        if (x + 1 < d->width) cur[x + 1] += (int16_t)(err * 7);
        if (x > 0) next[x - 1] += (int16_t)(err * 3);
        next[x] += (int16_t)(err * 5);
        if (x + 1 < d->width) next[x + 1] += (int16_t)err;
    }
    d->y++;
}
