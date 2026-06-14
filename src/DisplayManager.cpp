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
#include <GxEPD2_BW.h>
#include <gdey/GxEPD2_370_GDEY037T03.h>
#include <SPI.h>
#include <SdFat.h>

using namespace Adafruit_LittleFS_Namespace;
using LfsFile = Adafruit_LittleFS_Namespace::File;

// Instantiate EPD driver in B&W mode
GxEPD2_BW<GxEPD2_370_GDEY037T03, GxEPD2_370_GDEY037T03::HEIGHT>
    epd(GxEPD2_370_GDEY037T03(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

static uint16_t getTextWidth(const char* str, const GFXfont* font) {
  if (!str || !font) return 0;
  uint16_t w = 0;
  while (*str) {
    uint8_t c = *str++;
    if (c >= font->first && c <= font->last) {
      w += font->glyph[c - font->first].xAdvance;
    } else if (c == ' ') {
      if (' ' >= font->first && ' ' <= font->last) {
        w += font->glyph[' ' - font->first].xAdvance;
      } else {
        w += 5;
      }
    }
  }
  return w;
}

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
    : _fontType(FONT_SANS), _fontSize(SIZE_MEDIUM), _isFlipped(false), _isWakeupFromSleep(false), _nextPageOffset(0), _displayNeedsReinit(false) {
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
  // Ensure SPI is configured and enabled for the display, as other
  // shared-bus devices (like the SD card) might have altered pin states.
  SPI.setPins(EPD_MISO, EPD_SCK, EPD_MOSI);
  SPI.begin();

  // If the peripherals were power-cycled to recover the SD card,
  // we must reinitialize the display registers.
  if (_displayNeedsReinit) {
    Serial.println("[Display Debug] Re-initializing display registers after power cycle...");
    epd.init(115200, false, 1, false);
    epd.setRotation(_isFlipped ? 3 : 1);
    _displayNeedsReinit = false;
  }
}

// History circular queue implementation
void DisplayManager::clearHistory() {
  _historyHead = 0;
  _historyTail = 0;
  _historyCount = 0;
  memset(_history, 0, sizeof(_history));
  clearPageCache();
}

void DisplayManager::clearPageCache() {
  _pageCacheLength = 0;
  _pageCacheFilename = "";
  _pageCacheStartOffset = 0;
  _pageCacheTrueStartOffset = 0;
  memset(_pageCacheBuffer, 0, sizeof(_pageCacheBuffer));
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

void DisplayManager::cachePageText(const String &filename, uint32_t startOffset) {
  // Check if we have a cache hit.
  // The cache hits if it's the same file, the startOffset is within the cached range,
  // and we have at least 1023 bytes remaining in the cache buffer OR we have cached to the end of the file.
  if (_pageCacheFilename == filename &&
      startOffset >= _pageCacheStartOffset &&
      startOffset < _pageCacheStartOffset + _pageCacheLength) {
    uint32_t remaining = (_pageCacheStartOffset + _pageCacheLength) - startOffset;
    if (remaining >= 1023 || _pageCacheLength < (PAGE_CACHE_SIZE - 1)) {
      // If startOffset was 0, we still need to set the trueStartOffset.
      // We parse the title line from the cache buffer to find the trueStartOffset.
      if (startOffset == 0) {
        char* newlinePtr = strchr(_pageCacheBuffer, '\n');
        if (newlinePtr) {
          _pageCacheTrueStartOffset = (newlinePtr - _pageCacheBuffer) + 1;
        } else {
          _pageCacheTrueStartOffset = 0;
        }
      } else {
        _pageCacheTrueStartOffset = startOffset;
      }
      return; // Cache hit!
    }
  }

  // Cache miss: clear old cache and fill from backend
  _pageCacheFilename = filename;
  _pageCacheStartOffset = startOffset;
  _pageCacheTrueStartOffset = startOffset;
  _pageCacheLength = 0;
  _pageCacheBuffer[0] = '\0';

  if (filename.startsWith("[BLE]")) {
    String realFilename = filename.substring(5);
    uint32_t fetchOffset = startOffset;
    
    // Fetch directly into the unified page cache
    int bytesRead = BleManager::getInstance().requestBookText(realFilename, fetchOffset, _pageCacheBuffer, PAGE_CACHE_SIZE - 1);
    if (bytesRead < 0) bytesRead = 0;
    _pageCacheBuffer[bytesRead] = '\0';
    _pageCacheLength = bytesRead;
    _pageCacheStartOffset = fetchOffset;
    
    if (startOffset == 0) {
      char* newlinePtr = strchr(_pageCacheBuffer, '\n');
      if (newlinePtr) {
        _pageCacheTrueStartOffset = (newlinePtr - _pageCacheBuffer) + 1;
      } else {
        _pageCacheTrueStartOffset = 0;
      }
    } else {
      _pageCacheTrueStartOffset = startOffset;
    }

  } else if (filename.startsWith("[SD]")) {
    String realFilename = filename.substring(4);
    
    for (int attempt = 1; attempt <= 2; attempt++) {
      Serial.print("[SD Debug] cachePageText: Opening file: ");
      Serial.print(realFilename);
      Serial.print(" (Attempt ");
      Serial.print(attempt);
      Serial.println(")");
      
      FsFile file = StorageManager::getInstance().openSDBook(realFilename, O_RDONLY);
      if (!file) {
        Serial.println("[SD Debug] cachePageText: Failed to open SD book");
        if (attempt == 1) {
          Serial.println("[SD Debug] cachePageText: Retrying SD init...");
          StorageManager::getInstance().forceSDReinit();
          continue;
        }
        return;
      }
      
      Serial.print("[SD Debug] cachePageText: File opened successfully. Size: ");
      Serial.println((uint32_t)file.size());
      
      if (!file.seek(startOffset)) {
        Serial.println("[SD Debug] cachePageText: Seek failed!");
      }
      
      int bytesRead = file.read((uint8_t *)_pageCacheBuffer, PAGE_CACHE_SIZE - 1);
      Serial.print("[SD Debug] cachePageText: bytesRead: ");
      Serial.println(bytesRead);
      
      if (bytesRead < 0) {
        Serial.print("[SD Debug] cachePageText: SD error code: 0x");
        Serial.println(StorageManager::getInstance().getSDErrorCode(), HEX);
        file.close();
        if (attempt == 1) {
          Serial.println("[SD Debug] cachePageText: Retrying SD init due to read error...");
          StorageManager::getInstance().forceSDReinit();
          continue;
        }
        bytesRead = 0;
      }
      
      _pageCacheLength = bytesRead;
      _pageCacheBuffer[_pageCacheLength] = '\0';
      file.close();
      
      if (startOffset == 0) {
        char* newlinePtr = strchr(_pageCacheBuffer, '\n');
        if (newlinePtr) {
          _pageCacheTrueStartOffset = (newlinePtr - _pageCacheBuffer) + 1;
        } else {
          _pageCacheTrueStartOffset = 0;
        }
      } else {
        _pageCacheTrueStartOffset = startOffset;
      }
      break; // Success
    }

  } else {
    // LittleFS
    LfsFile file = StorageManager::getInstance().openBook(filename, "r");
    if (!file) {
      Serial.println("[Flash Debug] cachePageText: Failed to open flash book");
      return;
    }
    
    file.seek(startOffset);
    int bytesRead = file.read((uint8_t *)_pageCacheBuffer, PAGE_CACHE_SIZE - 1);
    if (bytesRead < 0) bytesRead = 0;
    _pageCacheLength = bytesRead;
    _pageCacheBuffer[_pageCacheLength] = '\0';
    file.close();
    
    if (startOffset == 0) {
      char* newlinePtr = strchr(_pageCacheBuffer, '\n');
      if (newlinePtr) {
        _pageCacheTrueStartOffset = (newlinePtr - _pageCacheBuffer) + 1;
      } else {
        _pageCacheTrueStartOffset = 0;
      }
    } else {
      _pageCacheTrueStartOffset = startOffset;
    }
  }
}

uint32_t DisplayManager::drawPageText(const String &filename,
                                      uint32_t startOffset,
                                      bool performRender) {
  // Ensure the requested page text is cached
  cachePageText(filename, startOffset);

  uint32_t trueStartOffset = _pageCacheTrueStartOffset;
  uint32_t cacheOffset = trueStartOffset - _pageCacheStartOffset;
  uint32_t copyLen = 0;
  if (_pageCacheLength > (int)cacheOffset) {
    copyLen = min((uint32_t)1023, (uint32_t)(_pageCacheLength - cacheOffset));
  }

  char buffer[1024];
  if (copyLen > 0) {
    memcpy(buffer, _pageCacheBuffer + cacheOffset, copyLen);
  }
  buffer[copyLen] = '\0';

  if (copyLen <= 0) {
    if (performRender) {
      epd.setFont(&AtkinsonHyperlegibleNext9pt7b);
      epd.setTextColor(GxEPD_BLACK);
      epd.setCursor(10, 45);
      epd.print("End of book reached.");
    }
    _nextPageOffset = trueStartOffset;
    return trueStartOffset;
  }

  preprocessUtf8(buffer, copyLen);

  const GFXfont *selectedFont = &AtkinsonHyperlegibleNext9pt7b;

  if (_fontType == FONT_SANS || _fontType == FONT_MONO || _fontType == FONT_ATKINSON) {
    if (_fontSize == SIZE_SMALL) {
      selectedFont = &AtkinsonHyperlegibleNext9pt7b;
    } else if (_fontSize == SIZE_MEDIUM) {
      selectedFont = &AtkinsonHyperlegibleNext12pt7b;
    } else if (_fontSize == SIZE_LARGE) {
      selectedFont = &AtkinsonHyperlegibleNext18pt7b;
    }
  } else if (_fontType == FONT_SERIF) {
    if (_fontSize == SIZE_SMALL) {
      selectedFont = &Bookerly9pt7b;
    } else if (_fontSize == SIZE_MEDIUM) {
      selectedFont = &Bookerly12pt7b;
    } else if (_fontSize == SIZE_LARGE) {
      selectedFont = &Bookerly18pt7b;
    }
  } else if (_fontType == FONT_LITERATA) {
    if (_fontSize == SIZE_SMALL) {
      selectedFont = &Literata9pt7b;
    } else if (_fontSize == SIZE_MEDIUM) {
      selectedFont = &Literata12pt7b;
    } else if (_fontSize == SIZE_LARGE) {
      selectedFont = &Literata18pt7b;
    }
  }

  int line_height = selectedFont->yAdvance;
  int y_start = line_height - 3;
  if (selectedFont == &AtkinsonHyperlegibleNext9pt7b || selectedFont == &AtkinsonHyperlegibleNext12pt7b) {
    y_start = line_height - 2;
  }

  int space_width = 5;
  if (' ' >= selectedFont->first && ' ' <= selectedFont->last) {
    space_width = selectedFont->glyph[' ' - selectedFont->first].xAdvance;
  }

  int x_min = 10;
  int x_max = epd.width() - 10;
  int y_end = epd.height() - 10;

  int cur_x = x_min;
  int cur_y = y_start;

  uint32_t idx = 0;
  // Skip any leading newlines and carriage returns at the start of the page
  // to prevent blank lines at the top of the display.
  while (idx < copyLen && (buffer[idx] == '\n' || buffer[idx] == '\r')) {
    idx++;
  }

  uint32_t wordStart = idx;
  bool inWord = false;

  if (performRender) {
    epd.setFont(selectedFont);
    epd.setTextColor(GxEPD_BLACK);
  }

  while (idx <= copyLen && cur_y <= y_end) {
    char c = buffer[idx];

    if (c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\0') {
      if (inWord) {
        char temp = buffer[idx];
        buffer[idx] = '\0';

        // Measure word width using our static helper
        uint16_t bw = getTextWidth(&buffer[wordStart], selectedFont);
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
          buffer[idx] = '\0';
          epd.setCursor(cur_x, cur_y);
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

void ReaderView::prepare() {
  DisplayManager::getInstance().cachePageText(_filename, _startOffset);
}

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
  display.setTextColor(GxEPD_BLACK);
  display.setFont(&AmazonEmber_Medium9pt7b);
  display.setCursor(10, 15);
  display.print(_header.c_str());
  display.drawFastHLine(0, 22, display.width(), GxEPD_BLACK);

  // Render Options within viewport
  int startY = 42;
  int endVisibleIdx = min(startVisibleIdx + maxVisible, _count);

  for (int i = startVisibleIdx; i < endVisibleIdx; i++) {
    int displayIdx = i - startVisibleIdx;
    int y = startY + displayIdx * itemHeight;
    display.setCursor(i == _selectedIdx ? 10 : 20, y);
    if (i == _selectedIdx) {
      String opt = "> " + _options[i];
      display.print(opt.c_str());
    } else {
      display.print(_options[i].c_str());
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
  display.setTextColor(_isAlert ? GxEPD_RED : GxEPD_BLACK);
  display.setFont(&AmazonEmber_Medium9pt7b);
  display.setCursor(10, 15);
  display.print(_title.c_str());
  display.drawFastHLine(0, 22, display.width(), GxEPD_BLACK);

  // Message body
  display.setTextColor(GxEPD_BLACK);

  int x = 10;
  int y = 45;

  for (size_t i = 0; i < _msg.length(); i++) {
    char c = _msg.charAt(i);
    if (c == '\n') {
      x = 10;
      y += 19;
    } else {
      int advance = 8;
      if (c >= AmazonEmber_Medium9pt7b.first && c <= AmazonEmber_Medium9pt7b.last) {
        advance = AmazonEmber_Medium9pt7b.glyph[c - AmazonEmber_Medium9pt7b.first].xAdvance;
      }
      if (x + advance > display.width() - 16) {
        x = 10;
        y += 19;
      }
      display.setCursor(x, y);
      display.write(c);
      x += advance;
    }
  }
}

// ProgressView
ProgressView::ProgressView(const String& task, int percentage)
    : _task(task), _percentage(percentage) {}

void ProgressView::render(Adafruit_GFX& display) {
  // Header
  display.setTextColor(GxEPD_BLACK);
  display.setFont(&AmazonEmber_Medium9pt7b);
  display.setCursor(10, 15);
  display.print("BLE File Upload");
  display.drawFastHLine(0, 22, display.width(), GxEPD_BLACK);

  // Task name
  display.setCursor(15, 50);
  display.print(_task.c_str());

  // Progress bar container
  int barWidth = 256;
  int barX = (display.width() - barWidth) / 2;
  display.drawRect(barX, 75, barWidth, 16, GxEPD_BLACK);

  // Progress fill
  int fillWidth = map(_percentage, 0, 100, 0, barWidth - 4);
  display.fillRect(barX + 2, 77, fillWidth, 12, GxEPD_BLACK);

  // Percentage text
  String pctStr = String(_percentage) + "%";
  display.setCursor(display.width() / 2 - 16, 110);
  display.print(pctStr.c_str());
}

// ==================== DisplayManager Draw Controller ====================

void DisplayManager::draw(UIView& view) {
  view.prepare();
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
  LfsFile file = InternalFS.open("/settings.dat", FILE_O_READ);
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
  LfsFile file = InternalFS.open("/settings.dat", FILE_O_WRITE);
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
  // 4. The remaining bytes from next page start offset to cache end is less than 1024 bytes.
  if (_pageCacheFilename == filename && 
      _pageCacheLength == (PAGE_CACHE_SIZE - 1) &&
      _nextPageOffset >= _pageCacheStartOffset &&
      _nextPageOffset < _pageCacheStartOffset + _pageCacheLength) {
      
    uint32_t remaining = (_pageCacheStartOffset + _pageCacheLength) - _nextPageOffset;
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
      int bytesRead = BleManager::getInstance().requestBookText(realFilename, _nextPageOffset, _pageCacheBuffer, PAGE_CACHE_SIZE - 1);
      _pageCacheBuffer[bytesRead] = '\0';
      _pageCacheStartOffset = _nextPageOffset;
      _pageCacheLength = bytesRead;
      _pageCacheFilename = filename;
      
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


