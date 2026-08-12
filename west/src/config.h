#ifndef CONFIG_H
#define CONFIG_H

#include <zephyr/kernel.h>
#include <stdbool.h>
#include <stdint.h>

// --- E-Paper Display Dimensions (GDEY037T03 3.7" E-Paper) ---
#define EPD_PHYS_WIDTH  240
#define EPD_PHYS_HEIGHT 416
#define EPD_BUFFER_SIZE ((EPD_PHYS_WIDTH * EPD_PHYS_HEIGHT) / 8) // 12,480 bytes

// Logical Landscape UI Dimensions
#define EPD_WIDTH  416
#define EPD_HEIGHT 240

// --- System Settings ---
#define MAX_BOOKS 10
#define MAX_CHAPTERS 100
#define HISTORY_SIZE 64
#define AUTO_SLEEP_MS 300000 // 5 minutes inactivity timeout
#define EPD_FULL_REFRESH_INTERVAL 20

// Path mountpoints
#define SD_MOUNT_POINT "/SD:"
#define LFS_MOUNT_POINT "/lfs:"
#define BOOK_DIR "/SD:/books"
#define PROGRESS_FILE "/SD:/progress.dat"
#define STATS_FILE "/SD:/stats.dat"

#endif // CONFIG_H
