#ifndef PREFS_H
#define PREFS_H

#include <stdbool.h>
#include <stdint.h>

// User preferences and stats, kept in internal flash (Zephyr settings on NVS) so they
// survive reboots and don't depend on the SD card. New fields go at the end, with explicit
// padding: a shorter blob from older firmware still loads and the new fields read as 0.
// Bump the key in prefs.c only when existing fields change.
typedef struct {
    uint8_t font;
    uint8_t size;
    uint8_t spacing;
    uint8_t contrast;
    uint8_t flipped;
    uint8_t refresh_profile;
    uint8_t pulse_interval;
    uint8_t pulse_count;
    uint32_t reading_seconds;
    // ---- end of the original 12-byte layout ----
    uint8_t frame_rate; // Hz, 0 = controller default
    uint8_t reserved[3];
} Prefs;

// Returns false when nothing has been stored yet
bool prefs_load(Prefs *out);
// Writes only when the values differ from what is already stored
int prefs_save(const Prefs *prefs);

#endif // PREFS_H
