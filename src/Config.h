#ifndef CONFIG_H
#define CONFIG_H

// --- E-Paper Display Pins (SPI + Control) ---
#define EPD_CS 33   // Maps to physical D10 (P0.09)
#define EPD_DC 28   // Maps to physical D3 (P0.20) - DC relocated here
#define EPD_RST 29  // Maps to physical D2 (P0.17) - RST relocated here
#define EPD_BUSY 23 // Maps to physical D7 (P0.11)

// --- SD Card Pins (SPI Shared with Display + Dedicated CS) ---
#define SD_CS 35    // Maps to physical D8 (P1.13)

// --- SPI Pins (Customized for available board pins D9/D16/D20) ---
#define EPD_MISO 3  // Maps to physical D9 (P1.15)
#define EPD_MOSI 2  // Maps to physical D16 (P0.10)
#define EPD_SCK 20  // Maps to physical D20 (P0.29)

// --- Hardware Button Pins ---
// Connect buttons between the pin and GND. The firmware uses internal pull-ups.
#define PIN_BTN_PREV 18 // Maps to physical D19 (P0.02) - Swapped in code to match hardware
#define PIN_BTN_NEXT                                                           \
  1 // Maps to physical D5 (P0.24) - Mapped to correct Index 1
#define PIN_BTN_SELECT                                                         \
  30 // Maps to physical D4 (P0.22) - Swapped in code to match hardware

// --- E-Paper Display Configuration ---
#define EPAPER_3COLOR                                                          \
  false // Set to false to run the 3-color panel in ultra-fast B&W mode with bypassed Red RAM
#define DISPLAY_ROTATION 1 // 1 = Landscape (416x240), 3 = Inverted Landscape

// --- Storage & Paging Engine Settings ---
#define MAX_BOOKS 10    // Maximum books stored in flash
#define HISTORY_SIZE 64 // Size of circular page offset cache (saves RAM)
#define BOOK_DIR "/books"
#define PROGRESS_FILE "/progress.dat"
#define TOTAL_FS_SIZE                                                          \
  (64 * 1024) // Total space allocated for InternalFS on nice!nano (64KB)

// --- Battery Monitoring ---
#define PIN_BATTERY A0 // Maps to physical P0.04 (AIN2) which is A0 on Feather
#define BATTERY_DIVIDER 2.0f // 1/2 voltage divider on nice!nano battery pin

// --- Power Control Settings ---
// Set to true if you have verified that Pin 13 controls your display's VCC
// MOSFET. Set to false to bypass Pin 13 control and keep the display powered
// permanently (safer default).
#define ENABLE_VCC_CONTROL true
#define PIN_VCC_ON 13 // VCC Power control pin

// --- System Auto-Sleep Timeout ---
#define AUTO_SLEEP_MS                                                          \
  120000 // 2 minutes of inactivity before deep sleep (system-off/light-sleep)

#endif // CONFIG_H
