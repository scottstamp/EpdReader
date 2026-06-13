#include "DisplayManager.h"
#include "Config.h"
#include "StorageManager.h"
#include "BleManager.h"

#include "Bookerly12pt7b.h"
#include "Bookerly18pt7b.h"
#include "Bookerly9pt7b.h"
#include "Bookerly_Bold9pt7b.h"
#include "Literata9pt7b.h"
#include "Literata12pt7b.h"
#include "Literata18pt7b.h"
#include "AmazonEmber_Medium9pt7b.h"
#include "AtkinsonHyperlegibleNext9pt7b.h"
#include "AtkinsonHyperlegibleNext12pt7b.h"
#include "AtkinsonHyperlegibleNext18pt7b.h"
#include <Fonts/FreeMono12pt7b.h>
#include <Fonts/FreeMono18pt7b.h>
#include <Fonts/FreeMono9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSans18pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <GxEPD2_3C.h>
#include <GxEPD2_BW.h>
#include <SPI.h>
#include <gdey/GxEPD2_370_GDEY037T03.h>
#include <epd3c/GxEPD2_290_C90c.h>

using namespace Adafruit_LittleFS_Namespace;

// Instantiate EPD driver as 'epd' to prevent global macro collisions
#if EPAPER_3COLOR
GxEPD2_3C<GxEPD2_290_C90c, GxEPD2_290_C90c::HEIGHT>
    epd(GxEPD2_290_C90c(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));
#else
GxEPD2_BW<GxEPD2_370_GDEY037T03, GxEPD2_370_GDEY037T03::HEIGHT>
    epd(GxEPD2_370_GDEY037T03(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));
#endif

#ifndef GxEPD_RED
#define GxEPD_RED GxEPD_BLACK
#endif

static void preprocessUtf8(char* buffer, int len) {
  for (int i = 0; i < len - 2; i++) {
    if ((uint8_t)buffer[i] == 0xE2 && (uint8_t)buffer[i + 1] == 0x80) {
      uint8_t third = (uint8_t)buffer[i + 2];
      if (third == 0x94) {         // em dash
        buffer[i] = 127;
        buffer[i + 1] = 0x01;
        buffer[i + 2] = 0x01;
        i += 2;
      } else if (third == 0x9C) {  // left double quote
        buffer[i] = 128;
        buffer[i + 1] = 0x01;
        buffer[i + 2] = 0x01;
        i += 2;
      } else if (third == 0x9D) {  // right double quote
        buffer[i] = 129;
        buffer[i + 1] = 0x01;
        buffer[i + 2] = 0x01;
        i += 2;
      } else if (third == 0x99) {  // right single quote / apostrophe
        buffer[i] = 130;
        buffer[i + 1] = 0x01;
        buffer[i + 2] = 0x01;
        i += 2;
      } else if (third == 0xA6) {  // ellipsis
        buffer[i] = 131;
        buffer[i + 1] = 0x01;
        buffer[i + 2] = 0x01;
        i += 2;
      }
    }
  }
}

DisplayManager &DisplayManager::getInstance() {
  static DisplayManager instance;
  return instance;
}

DisplayManager::DisplayManager()
    : _fontType(FONT_SANS), _fontSize(SIZE_MEDIUM), _isFlipped(false), _isWakeupFromSleep(false), _nextPageOffset(0) {
  clearHistory();
}

void DisplayManager::begin() {
  // Set custom SPI pins before initialization to support custom board pinouts
  SPI.setPins(EPD_MISO, EPD_SCK, EPD_MOSI);

  // Read the general purpose retention register to identify deep sleep wakeups
  uint32_t gpregret = NRF_POWER->GPREGRET;

  // Check if the magic retention flag (0x55) is set
  if (gpregret == 0x55) {
    _isWakeupFromSleep = true;
    NRF_POWER->GPREGRET = 0; // Clear the flag
  } else {
    _isWakeupFromSleep = false;
  }

  Serial.print("[Display Debug] GPREGRET register: 0x");
  Serial.print(gpregret, HEX);
  if (_isWakeupFromSleep) {
    Serial.println(" -> Woke up from Deep Sleep.");
  } else {
    Serial.println(" -> Cold Power-On / Pin Reset.");
  }

  // GxEPD2 setup
  epd.init(115200, !_isWakeupFromSleep, 1,
           false); // SPI init (initial=false on sleep wakeup to bypass forced
                   // full refresh)
  loadSettings();
  epd.setRotation(_isFlipped ? 3 : 1);

  // Clean clear on boot only if NOT waking up from deep sleep.
  // If waking up from sleep, the display is already showing the correct text
  // from the last session.
  if (!_isWakeupFromSleep) {
    clear();
  }
}

void DisplayManager::clear() {
  powerUp();
  epd.firstPage();
  do {
    epd.fillScreen(GxEPD_WHITE);
  } while (epd.nextPage());
  powerDown();
}

void DisplayManager::powerDown() { epd.powerOff(); }

void DisplayManager::powerUp() {
  // If SPI pins were tri-stated or put to sleep, epd.init handles it
}

// History circular queue implementation
void DisplayManager::clearHistory() {
  _historyHead = 0;
  _historyTail = 0;
  _historyCount = 0;
  memset(_history, 0, sizeof(_history));
  clearBleCache();
}

void DisplayManager::clearBleCache() {
  _bleCacheStartOffset = 0;
  _bleCacheLength = 0;
  _bleCachedFilename = "";
}

void DisplayManager::pushHistory(uint32_t offset) {
  if (_historyCount > 0) {
    int lastIdx = (_historyHead - 1 + HISTORY_SIZE) % HISTORY_SIZE;
    if (_history[lastIdx] == offset) {
      return; // Don't push duplicate offsets
    }
  }

  _history[_historyHead] = offset;
  _historyHead = (_historyHead + 1) % HISTORY_SIZE;

  if (_historyCount < HISTORY_SIZE) {
    _historyCount++;
  } else {
    _historyTail = (_historyTail + 1) % HISTORY_SIZE; // Overwrote tail
  }
}

uint32_t DisplayManager::popHistory() {
  if (_historyCount <= 1) {
    if (_historyCount == 1) {
      return _history[_historyTail]; // Stay on the first page
    }
    return 0;
  }

  // Step back from head (current page) to get to the previous page
  _historyHead = (_historyHead - 1 + HISTORY_SIZE) % HISTORY_SIZE;
  _historyCount--;

  int prevIdx = (_historyHead - 1 + HISTORY_SIZE) % HISTORY_SIZE;
  return _history[prevIdx];
}

bool DisplayManager::hasHistory() const { return _historyCount > 1; }

int DisplayManager::getHistoryOffsets(uint32_t* dest, int maxLen) {
  int count = min(_historyCount, maxLen);
  for (int i = 0; i < count; i++) {
    int idx = (_historyTail + i) % HISTORY_SIZE;
    dest[i] = _history[idx];
  }
  return count;
}

void DisplayManager::setHistoryOffsets(const uint32_t* src, int count) {
  clearHistory();
  for (int i = 0; i < count; i++) {
    pushHistory(src[i]);
  }
}

uint32_t DisplayManager::drawPageText(const String &filename,
                                      uint32_t startOffset,
                                      bool performRender) {
  uint32_t trueStartOffset = startOffset;
  char buffer[1024];
  int len = 0;

  if (filename.startsWith("[BLE]")) {
    String realFilename = filename.substring(5);
    if (startOffset == 0) {
      // For offset 0, ensure we have the first block in the cache to find the title line
      if (_bleCachedFilename != realFilename || _bleCacheLength == 0 || _bleCacheStartOffset != 0) {
        int bytesRead = BleManager::getInstance().requestBookText(realFilename, 0, _bleCacheBuffer, BLE_CACHE_SIZE - 1);
        _bleCacheBuffer[bytesRead] = '\0';
        _bleCacheStartOffset = 0;
        _bleCacheLength = bytesRead;
        _bleCachedFilename = realFilename;
      }
      
      char* newlinePtr = strchr(_bleCacheBuffer, '\n');
      if (newlinePtr) {
        trueStartOffset = (newlinePtr - _bleCacheBuffer) + 1;
      } else {
        trueStartOffset = 0;
      }
    }

    // Check if the required page text is within our cached range
    bool cacheHit = false;
    if (_bleCachedFilename == realFilename && 
        trueStartOffset >= _bleCacheStartOffset && 
        trueStartOffset < _bleCacheStartOffset + _bleCacheLength) {
        
      uint32_t remaining = (_bleCacheStartOffset + _bleCacheLength) - trueStartOffset;
      // We have a hit if we have at least a page's worth of bytes OR we reached EOF
      if (remaining >= (sizeof(buffer) - 1) || _bleCacheLength < (BLE_CACHE_SIZE - 1)) {
        cacheHit = true;
      }
    }

    if (!cacheHit) {
      // Cache miss! Load new block starting at trueStartOffset
      int bytesRead = BleManager::getInstance().requestBookText(realFilename, trueStartOffset, _bleCacheBuffer, BLE_CACHE_SIZE - 1);
      _bleCacheBuffer[bytesRead] = '\0';
      _bleCacheStartOffset = trueStartOffset;
      _bleCacheLength = bytesRead;
      _bleCachedFilename = realFilename;
    }

    // Copy from the cached buffer to output buffer
    uint32_t cacheOffset = trueStartOffset - _bleCacheStartOffset;
    uint32_t copyLen = min((uint32_t)(sizeof(buffer) - 1), _bleCacheLength - cacheOffset);
    memcpy(buffer, _bleCacheBuffer + cacheOffset, copyLen);
    buffer[copyLen] = '\0';
    len = copyLen;
  } else {
    File file = StorageManager::getInstance().openBook(filename, "r");
    if (!file) {
      if (performRender) {
        epd.setCursor(10, 45);
        epd.print("Failed to open file.");
      }
      _nextPageOffset = startOffset;
      return startOffset;
    }

    if (startOffset == 0) {
      String titleLine = file.readStringUntil('\n');
      trueStartOffset = titleLine.length() + 1;
    }

    file.seek(trueStartOffset);
    len = file.read((uint8_t *)buffer, sizeof(buffer) - 1);
    buffer[len] = '\0';
    file.close();
  }

  if (len <= 0) {
    if (performRender) {
      epd.setCursor(10, 45);
      epd.print("End of book reached.");
    }
    _nextPageOffset = trueStartOffset;
    return trueStartOffset;
  }

  preprocessUtf8(buffer, len);

  const GFXfont *selectedFont = &FreeSans9pt7b;
  int line_height = 18;
  int y_start = 16;
  int space_width = 5;

  if (_fontType == FONT_SANS) {
    if (_fontSize == SIZE_SMALL) {
      selectedFont = &FreeSans9pt7b;
      line_height = 18;
      y_start = 16;
      space_width = 5;
    } else if (_fontSize == SIZE_MEDIUM) {
      selectedFont = &FreeSans12pt7b;
      line_height = 24;
      y_start = 22;
      space_width = 7;
    } else if (_fontSize == SIZE_LARGE) {
      selectedFont = &FreeSans18pt7b;
      line_height = 36;
      y_start = 32;
      space_width = 10;
    }
  } else if (_fontType == FONT_SERIF) {
    if (_fontSize == SIZE_SMALL) {
      selectedFont = &Bookerly9pt7b;
      line_height = 20;
      y_start = 16;
      space_width = 5;
    } else if (_fontSize == SIZE_MEDIUM) {
      selectedFont = &Bookerly12pt7b;
      line_height = 25;
      y_start = 22;
      space_width = 7;
    } else if (_fontSize == SIZE_LARGE) {
      selectedFont = &Bookerly18pt7b;
      line_height = 35;
      y_start = 32;
      space_width = 10;
    }
  } else if (_fontType == FONT_MONO) {
    if (_fontSize == SIZE_SMALL) {
      selectedFont = &FreeMono9pt7b;
      line_height = 18;
      y_start = 16;
      space_width = 8;
    } else if (_fontSize == SIZE_MEDIUM) {
      selectedFont = &FreeMono12pt7b;
      line_height = 24;
      y_start = 22;
      space_width = 10;
    } else if (_fontSize == SIZE_LARGE) {
      selectedFont = &FreeMono18pt7b;
      line_height = 36;
      y_start = 32;
      space_width = 15;
    }
  } else if (_fontType == FONT_LITERATA) {
    if (_fontSize == SIZE_SMALL) {
      selectedFont = &Literata9pt7b;
      line_height = 22;
      y_start = 18;
      space_width = 5;
    } else if (_fontSize == SIZE_MEDIUM) {
      selectedFont = &Literata12pt7b;
      line_height = 28;
      y_start = 24;
      space_width = 7;
    } else if (_fontSize == SIZE_LARGE) {
      selectedFont = &Literata18pt7b;
      line_height = 40;
      y_start = 34;
      space_width = 10;
    }
  } else if (_fontType == FONT_ATKINSON) {
    if (_fontSize == SIZE_SMALL) {
      selectedFont = &AtkinsonHyperlegibleNext9pt7b;
      line_height = 16;
      y_start = 14;
      space_width = 5;
    } else if (_fontSize == SIZE_MEDIUM) {
      selectedFont = &AtkinsonHyperlegibleNext12pt7b;
      line_height = 20;
      y_start = 18;
      space_width = 7;
    } else if (_fontSize == SIZE_LARGE) {
      selectedFont = &AtkinsonHyperlegibleNext18pt7b;
      line_height = 28;
      y_start = 25;
      space_width = 10;
    }
  }

  epd.setFont(selectedFont);
  epd.setTextColor(GxEPD_BLACK);

  int x_min = 10;
  int x_max = epd.width() - 10;
  int y_end = epd.height() - 10;

  int cur_x = x_min;
  int cur_y = y_start;

  int idx = 0;
  // Skip any leading newlines and carriage returns at the start of the page
  // to prevent blank lines at the top of the display.
  while (idx < len && (buffer[idx] == '\n' || buffer[idx] == '\r')) {
    idx++;
  }

  int wordStart = idx;
  bool inWord = false;

  while (idx <= len && cur_y <= y_end) {
    char c = buffer[idx];

    if (c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\0') {
      if (inWord) {
        char temp = buffer[idx];
        buffer[idx] = '\0';

        // Measure word width
        int16_t bx, by;
        uint16_t bw, bh;
        epd.getTextBounds(&buffer[wordStart], 0, 0, &bx, &by, &bw, &bh);
        buffer[idx] = temp;

        // Wrap if it exceeds bounds
        if (cur_x + bw > x_max) {
          cur_x = x_min;
          cur_y += line_height;
        }

        if (cur_y > y_end) {
          idx = wordStart; // Start from this word next page
          break;
        }

        if (performRender) {
          epd.setCursor(cur_x, cur_y);
          buffer[idx] = '\0';
          epd.print(&buffer[wordStart]);
          buffer[idx] = temp;
        }

        cur_x += bw;
        inWord = false;
      }

      if (c == '\n') {
        cur_x = x_min;
        cur_y += line_height;
      } else if (c == ' ') {
        cur_x += space_width;
      } else if (c == '\0') {
        break;
      }

      idx++;
    } else {
      if (!inWord) {
        wordStart = idx;
        inWord = true;
      }
      idx++;
    }
  }

  _nextPageOffset = trueStartOffset + idx;
  return _nextPageOffset;
}

// ==================== UIView Implementations ====================

// ReaderView
ReaderView::ReaderView(const String& filename, uint32_t startOffset, uint32_t& nextPageOffset)
    : _filename(filename), _startOffset(startOffset), _nextPageOffset(nextPageOffset) {}

void ReaderView::render(Adafruit_GFX& display) {
  _nextPageOffset = DisplayManager::getInstance().drawPageText(_filename, _startOffset, true);
}

bool ReaderView::prefersFullRefresh() {
  static int pageTurnCount = -1;
  if (pageTurnCount == -1) {
    pageTurnCount = DisplayManager::getInstance().isWakeupFromSleep() ? 1 : 0;
  }
  bool forceFull = (pageTurnCount % 6 == 0);
  pageTurnCount++;
  return forceFull;
}

// MenuView
MenuView::MenuView(const String& header, const String options[], int count, int selectedIdx)
    : _header(header), _options(options), _count(count), _selectedIdx(selectedIdx) {}

void MenuView::render(Adafruit_GFX& display) {
  int itemHeight = 19;
  int maxVisible = (display.height() - 42) / itemHeight;
  int startVisibleIdx = 0;
  if (_count > maxVisible) {
    if (_selectedIdx < maxVisible - 1) {
      startVisibleIdx = 0;
    } else if (_selectedIdx >= _count - 1) {
      startVisibleIdx = _count - maxVisible;
    } else {
      startVisibleIdx = _selectedIdx - (maxVisible / 2);
      if (startVisibleIdx < 0)
        startVisibleIdx = 0;
      if (startVisibleIdx > _count - maxVisible)
        startVisibleIdx = _count - maxVisible;
    }
  }

  // Header
  display.setFont(&AmazonEmber_Medium9pt7b);
  display.setTextColor(GxEPD_BLACK);
  display.setCursor(10, 15);
  display.print(_header);
  display.drawFastHLine(0, 22, display.width(), GxEPD_BLACK);

  // Render Options within viewport
  display.setFont(&AmazonEmber_Medium9pt7b);
  int startY = 42;
  int endVisibleIdx = min(startVisibleIdx + maxVisible, _count);

  for (int i = startVisibleIdx; i < endVisibleIdx; i++) {
    int displayIdx = i - startVisibleIdx;
    int y = startY + displayIdx * itemHeight;
    if (i == _selectedIdx) {
      display.setCursor(10, y);
      display.print("> " + _options[i]);
    } else {
      display.setCursor(20, y);
      display.print(_options[i]);
    }
  }

  // Draw scroll indicators on the right edge if there are items off-screen
  if (_count > maxVisible) {
    int right_edge = display.width() - 16;
    if (startVisibleIdx > 0) {
      // Up indicator arrow
      int upY = startY - 4;
      display.fillTriangle(right_edge, upY, right_edge + 4, upY - 6, right_edge + 8, upY, GxEPD_BLACK);
    }
    if (startVisibleIdx + maxVisible < _count) {
      // Down indicator arrow
      int downY = startY + maxVisible * itemHeight;
      display.fillTriangle(right_edge, downY, right_edge + 4, downY + 6, right_edge + 8, downY, GxEPD_BLACK);
    }
  }
}

// MessageView
MessageView::MessageView(const String& title, const String& msg, bool isAlert)
    : _title(title), _msg(msg), _isAlert(isAlert) {}

void MessageView::render(Adafruit_GFX& display) {
  // Header
  display.setFont(&AmazonEmber_Medium9pt7b);
  display.setTextColor(_isAlert ? GxEPD_RED : GxEPD_BLACK);
  display.setCursor(10, 15);
  display.print(_title);
  display.drawFastHLine(0, 22, display.width(), GxEPD_BLACK);

  // Message body
  display.setFont(&AmazonEmber_Medium9pt7b);
  display.setTextColor(GxEPD_BLACK);

  int x = 10;
  int y = 45;
  display.setCursor(x, y);

  for (size_t i = 0; i < _msg.length(); i++) {
    char c = _msg.charAt(i);
    if (c == '\n') {
      x = 10;
      y += 19;
      display.setCursor(x, y);
    } else {
      display.print(c);
      x += 8;
      if (x > display.width() - 16) {
        x = 10;
        y += 19;
        display.setCursor(x, y);
      }
    }
  }
}

// ProgressView
ProgressView::ProgressView(const String& task, int percentage)
    : _task(task), _percentage(percentage) {}

void ProgressView::render(Adafruit_GFX& display) {
  // Header
  display.setFont(&AmazonEmber_Medium9pt7b);
  display.setTextColor(GxEPD_BLACK);
  display.setCursor(10, 15);
  display.print("BLE File Upload");
  display.drawFastHLine(0, 22, display.width(), GxEPD_BLACK);

  // Task name
  display.setFont(&AmazonEmber_Medium9pt7b);
  display.setCursor(15, 50);
  display.print(_task);

  // Progress bar container
  int barWidth = 256;
  int barX = (display.width() - barWidth) / 2;
  display.drawRect(barX, 75, barWidth, 16, GxEPD_BLACK);

  // Progress fill
  int fillWidth = map(_percentage, 0, 100, 0, barWidth - 4);
  display.fillRect(barX + 2, 77, fillWidth, 12, GxEPD_BLACK);

  // Percentage text
  display.setFont(&AmazonEmber_Medium9pt7b);
  display.setCursor(display.width() / 2 - 16, 110);
  display.print(String(_percentage) + "%");
}

// ==================== DisplayManager Draw Controller ====================

void DisplayManager::draw(UIView& view) {
  powerUp();
  bool forceFull = view.prefersFullRefresh() || EPAPER_3COLOR;
  if (!forceFull) {
    epd.setPartialWindow(0, 0, epd.width(), epd.height());
  }
  epd.firstPage();
  do {
    epd.fillScreen(GxEPD_WHITE);
    view.render(epd);
  } while (epd.nextPage());
  powerDown();
}

void DisplayManager::setFontType(FontType type) {
  _fontType = type;
  saveSettings();
}

void DisplayManager::setFontSize(FontSize size) {
  _fontSize = size;
  saveSettings();
}

void DisplayManager::cycleFontType() {
  _fontType = (FontType)((_fontType + 1) % 5);
  saveSettings();
}

void DisplayManager::cycleFontSize() {
  _fontSize = (FontSize)((_fontSize + 1) % 3);
  saveSettings();
}

void DisplayManager::setFlipped(bool flipped) {
  _isFlipped = flipped;
  epd.setRotation(_isFlipped ? 3 : 1);
  saveSettings();
}

void DisplayManager::loadSettings() {
  // Set defaults first
  _fontType = FONT_SANS;
  _fontSize = SIZE_MEDIUM;
  _isFlipped = false;

  if (!InternalFS.exists("/settings.dat")) {
    return;
  }
  File file = InternalFS.open("/settings.dat", FILE_O_READ);
  if (file) {
    if (file.available()) {
      String typeStr = file.readStringUntil('\n');
      typeStr.trim();
      if (typeStr.length() > 0) {
        int val = typeStr.toInt();
        if (val >= 0 && val <= 4) {
          _fontType = (FontType)val;
        }
      }
    }
    if (file.available()) {
      String sizeStr = file.readStringUntil('\n');
      sizeStr.trim();
      if (sizeStr.length() > 0) {
        int val = sizeStr.toInt();
        if (val >= 0 && val <= 2) {
          _fontSize = (FontSize)val;
        }
      }
    }
    if (file.available()) {
      String flipStr = file.readStringUntil('\n');
      flipStr.trim();
      if (flipStr.length() > 0) {
        _isFlipped = (flipStr.toInt() == 1);
      }
    }
    file.close();
  }
}

void DisplayManager::saveSettings() {
  if (InternalFS.exists("/settings.dat")) {
    InternalFS.remove("/settings.dat");
  }
  File file = InternalFS.open("/settings.dat", FILE_O_WRITE);
  if (file) {
    file.println((int)_fontType);
    file.println((int)_fontSize);
    file.println(_isFlipped ? 1 : 0);
    file.close();
  }
}

void DisplayManager::checkAndTriggerPreFetch(const String& filename) {
  if (!filename.startsWith("[BLE]")) return;
  String realFilename = filename.substring(5);

  // Trigger pre-fetch if:
  // 1. We are currently reading the same book and have an active cache.
  // 2. The cache was fully populated (meaning there is likely more content on the PC).
  // 3. The next page start offset is within the current cache.
  // 4. The remaining bytes from next page start offset to cache end is less than 256.
  if (_bleCachedFilename == realFilename && 
      _bleCacheLength == (BLE_CACHE_SIZE - 1) &&
      _nextPageOffset >= _bleCacheStartOffset &&
      _nextPageOffset < _bleCacheStartOffset + _bleCacheLength) {
      
    uint32_t remaining = (_bleCacheStartOffset + _bleCacheLength) - _nextPageOffset;
    if (remaining < 1024) {
      Serial.print("[BLE Cache] Pre-fetching next block starting at offset ");
      Serial.println(_nextPageOffset);
      
      // 1. Draw a small clock/loading indicator in the top-right corner
      // using a fast, non-flashing partial update
      int loaderX = epd.width() - 21;
      powerUp();
      epd.setPartialWindow(loaderX, 0, 21, 20);
      epd.firstPage();
      do {
        epd.fillRect(loaderX, 0, 21, 20, GxEPD_WHITE);
        // Draw clock/sync icon
        epd.drawCircle(loaderX + 10, 10, 4, GxEPD_BLACK);
        epd.drawLine(loaderX + 10, 10, loaderX + 10, 7, GxEPD_BLACK);
        epd.drawLine(loaderX + 10, 10, loaderX + 13, 10, GxEPD_BLACK);
      } while (epd.nextPage());
      powerDown();

      // 2. Fetch the next block from PC (takes ~1.5s)
      int bytesRead = BleManager::getInstance().requestBookText(realFilename, _nextPageOffset, _bleCacheBuffer, BLE_CACHE_SIZE - 1);
      _bleCacheBuffer[bytesRead] = '\0';
      _bleCacheStartOffset = _nextPageOffset;
      _bleCacheLength = bytesRead;
      _bleCachedFilename = realFilename;
      
      // 3. Erase the loading indicator using partial-window refresh
      powerUp();
      epd.setPartialWindow(loaderX, 0, 21, 20);
      epd.firstPage();
      do {
        epd.fillRect(loaderX, 0, 21, 20, GxEPD_WHITE);
      } while (epd.nextPage());
      powerDown();
      
      Serial.println("[BLE Cache] Pre-fetch complete.");
    }
  }
}
