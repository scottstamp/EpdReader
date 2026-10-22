#include "GxEPD2_4G_370_GDEY037T03.h"

GxEPD2_4G_370_GDEY037T03::GxEPD2_4G_370_GDEY037T03(int16_t cs, int16_t dc, int16_t rst, int16_t busy) :
  GxEPD2_4G_EPD(cs, dc, rst, busy, LOW, 10000000, WIDTH, HEIGHT, panel, hasColor, hasPartialUpdate, hasFastPartialUpdate),
  _grayscale_mode(false)
{
}

void GxEPD2_4G_370_GDEY037T03::clearScreen(uint8_t value)
{
  _grayscale_mode = false;
  _writeScreenBuffer(0x10, value); // set previous
  _writeScreenBuffer(0x13, value); // set current
  refresh(false); // full refresh
  _initial_write = false;
}

void GxEPD2_4G_370_GDEY037T03::writeScreenBuffer(uint8_t value)
{
  _grayscale_mode = false;
  if (_initial_write) return clearScreen(value);
  _writeScreenBuffer(0x13, value); // set current
}

void GxEPD2_4G_370_GDEY037T03::writeScreenBufferAgain(uint8_t value)
{
  _grayscale_mode = false;
  _writeScreenBuffer(0x10, value); // set previous
}

void GxEPD2_4G_370_GDEY037T03::_writeScreenBuffer(uint8_t command, uint8_t value)
{
  if (!_init_display_done) _InitDisplay();
  _writeCommand(command);
  _startTransfer();
  for (uint32_t i = 0; i < uint32_t(WIDTH) * uint32_t(HEIGHT) / 8; i++)
  {
    _transfer(value);
  }
  _endTransfer();
}

void GxEPD2_4G_370_GDEY037T03::writeImage(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  _grayscale_mode = false;
  _writeImage(0x13, bitmap, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_4G_370_GDEY037T03::writeImageForFullRefresh(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  _grayscale_mode = false;
  _writeImage(0x10, bitmap, x, y, w, h, invert, mirror_y, pgm); // set previous
  _writeImage(0x13, bitmap, x, y, w, h, invert, mirror_y, pgm); // set current
}

void GxEPD2_4G_370_GDEY037T03::writeImageAgain(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  _grayscale_mode = false;
  _writeImage(0x10, bitmap, x, y, w, h, invert, mirror_y, pgm); // set previous
}

void GxEPD2_4G_370_GDEY037T03::writeImageToPrevious(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  _grayscale_mode = false;
  _writeImage(0x10, bitmap, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_4G_370_GDEY037T03::_writeImage(uint8_t command, const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  delay(1); // yield() to avoid WDT on ESP8266 and ESP32
  uint16_t wb = (w + 7) / 8; // width bytes, bitmaps are padded
  x -= x % 8; // byte boundary
  w = wb * 8; // byte boundary
  int16_t x1 = x < 0 ? 0 : x; // limit
  int16_t y1 = y < 0 ? 0 : y; // limit
  int16_t w1 = x + w < int16_t(WIDTH) ? w : int16_t(WIDTH) - x; // limit
  int16_t h1 = y + h < int16_t(HEIGHT) ? h : int16_t(HEIGHT) - y; // limit
  int16_t dx = x1 - x;
  int16_t dy = y1 - y;
  w1 -= dx;
  h1 -= dy;
  if ((w1 <= 0) || (h1 <= 0)) return;
  if (!_init_display_done) _InitDisplay();
  if (_initial_write) writeScreenBuffer(); // initial full screen buffer clean
  _writeCommand(0x91); // partial in
  _setPartialRamArea(x1, y1, w1, h1);
  _writeCommand(command);
  _startTransfer();
  for (int16_t i = 0; i < h1; i++)
  {
    for (int16_t j = 0; j < w1 / 8; j++)
    {
      uint8_t data;
      // use wb, h of bitmap for index!
      uint16_t idx = mirror_y ? j + dx / 8 + uint16_t((h - 1 - (i + dy))) * wb : j + dx / 8 + uint16_t(i + dy) * wb;
      if (pgm)
      {
#if defined(__AVR) || defined(ESP8266) || defined(ESP32)
        data = pgm_read_byte(&bitmap[idx]);
#else
        data = bitmap[idx];
#endif
      }
      else
      {
        data = bitmap[idx];
      }
      if (invert) data = ~data;
      _transfer(data);
    }
  }
  _endTransfer();
  _writeCommand(0x92); // partial out
  delay(1); // yield() to avoid WDT on ESP8266 and ESP32
}

void GxEPD2_4G_370_GDEY037T03::writeImagePart(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
    int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  _grayscale_mode = false;
  _writeImagePart(0x13, bitmap, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_4G_370_GDEY037T03::writeImagePartAgain(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
    int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  _grayscale_mode = false;
  _writeImagePart(0x10, bitmap, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm); // set previous
}

void GxEPD2_4G_370_GDEY037T03::writeImagePartToPrevious(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
    int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  _grayscale_mode = false;
  _writeImagePart(0x10, bitmap, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_4G_370_GDEY037T03::_writeImagePart(uint8_t command, const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
    int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  delay(1); // yield() to avoid WDT on ESP8266 and ESP32
  if ((w_bitmap < 0) || (h_bitmap < 0) || (w < 0) || (h < 0)) return;
  if ((x_part < 0) || (x_part >= w_bitmap)) return;
  if ((y_part < 0) || (y_part >= h_bitmap)) return;
  uint16_t wb_bitmap = (w_bitmap + 7) / 8; // width bytes, bitmaps are padded
  x_part -= x_part % 8; // byte boundary
  w = w_bitmap - x_part < w ? w_bitmap - x_part : w; // limit
  h = h_bitmap - y_part < h ? h_bitmap - y_part : h; // limit
  x -= x % 8; // byte boundary
  w = 8 * ((w + 7) / 8); // byte boundary, bitmaps are padded
  int16_t x1 = x < 0 ? 0 : x; // limit
  int16_t y1 = y < 0 ? 0 : y; // limit
  int16_t w1 = x + w < int16_t(WIDTH) ? w : int16_t(WIDTH) - x; // limit
  int16_t h1 = y + h < int16_t(HEIGHT) ? h : int16_t(HEIGHT) - y; // limit
  int16_t dx = x1 - x;
  int16_t dy = y1 - y;
  w1 -= dx;
  h1 -= dy;
  if ((w1 <= 0) || (h1 <= 0)) return;
  if (!_init_display_done) _InitDisplay();
  if (_initial_write) writeScreenBuffer(); // initial full screen buffer clean
  _writeCommand(0x91); // partial in
  _setPartialRamArea(x1, y1, w1, h1);
  _writeCommand(command);
  _startTransfer();
  for (int16_t i = 0; i < h1; i++)
  {
    for (int16_t j = 0; j < w1 / 8; j++)
    {
      uint8_t data;
      // use wb_bitmap, h_bitmap of bitmap for index!
      uint16_t idx = mirror_y ? x_part / 8 + j + dx / 8 + uint16_t((h_bitmap - 1 - (y_part + i + dy))) * wb_bitmap : x_part / 8 + j + dx / 8 + uint16_t(y_part + i + dy) * wb_bitmap;
      if (pgm)
      {
#if defined(__AVR) || defined(ESP8266) || defined(ESP32)
        data = pgm_read_byte(&bitmap[idx]);
#else
        data = bitmap[idx];
#endif
      }
      else
      {
        data = bitmap[idx];
      }
      if (invert) data = ~data;
      _transfer(data);
    }
  }
  _endTransfer();
  _writeCommand(0x92); // partial out
  delay(1); // yield() to avoid WDT on ESP8266 and ESP32
}

void GxEPD2_4G_370_GDEY037T03::writeImage(const uint8_t* black, const uint8_t* color, int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  _grayscale_mode = false;
  if (black)
  {
    writeImage(black, x, y, w, h, invert, mirror_y, pgm);
  }
}

void GxEPD2_4G_370_GDEY037T03::writeImagePart(const uint8_t* black, const uint8_t* color, int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
    int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  _grayscale_mode = false;
  if (black)
  {
    writeImagePart(black, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
  }
}

void GxEPD2_4G_370_GDEY037T03::drawNative(const uint8_t* data1, const uint8_t* data2, int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  if (_grayscale_mode && data2)
  {
    _writeImage(0x10, data1, x, y, w, h, invert, mirror_y, pgm); // LSB
    _writeImage(0x13, data2, x, y, w, h, invert, mirror_y, pgm); // MSB
  }
  else
  {
    _grayscale_mode = false;
    if (data1)
    {
      _writeImage(0x13, data1, x, y, w, h, invert, mirror_y, pgm);
    }
  }
}


void GxEPD2_4G_370_GDEY037T03::drawImage(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  _grayscale_mode = false;
  writeImage(bitmap, x, y, w, h, invert, mirror_y, pgm);
  refresh(x, y, w, h);
  writeImageAgain(bitmap, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_4G_370_GDEY037T03::drawImagePart(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
    int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  _grayscale_mode = false;
  writeImagePart(bitmap, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
  refresh(x, y, w, h);
  writeImagePartAgain(bitmap, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_4G_370_GDEY037T03::drawImage(const uint8_t* black, const uint8_t* color, int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  _grayscale_mode = false;
  if (black)
  {
    drawImage(black, x, y, w, h, invert, mirror_y, pgm);
  }
}

void GxEPD2_4G_370_GDEY037T03::drawImagePart(const uint8_t* black, const uint8_t* color, int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
    int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  _grayscale_mode = false;
  if (black)
  {
    drawImagePart(black, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
  }
}

void GxEPD2_4G_370_GDEY037T03::refresh(bool partial_update_mode)
{
  if (partial_update_mode) refresh(0, 0, WIDTH, HEIGHT);
  else
  {
    _Update_Full();
    _initial_refresh = false; // initial full update done
  }
}

void GxEPD2_4G_370_GDEY037T03::refresh(int16_t x, int16_t y, int16_t w, int16_t h)
{
  if (_initial_refresh) return refresh(false); // initial update needs be full update
  // intersection with screen
  int16_t w1 = x < 0 ? w + x : w; // reduce
  int16_t h1 = y < 0 ? h + y : h; // reduce
  int16_t x1 = x < 0 ? 0 : x; // limit
  int16_t y1 = y < 0 ? 0 : y; // limit
  w1 = x1 + w1 < int16_t(WIDTH) ? w1 : int16_t(WIDTH) - x1; // limit
  h1 = y1 + h1 < int16_t(HEIGHT) ? h1 : int16_t(HEIGHT) - y1; // limit
  if ((w1 <= 0) || (h1 <= 0)) return;
  // make x1, w1 multiple of 8
  w1 += x1 % 8;
  if (w1 % 8 > 0) w1 += 8 - w1 % 8;
  x1 -= x1 % 8;
  if (usePartialUpdateWindow) _writeCommand(0x91); // partial in
  _setPartialRamArea(x1, y1, w1, h1);
  _Update_Part();
  if (usePartialUpdateWindow) _writeCommand(0x92); // partial out
}

void GxEPD2_4G_370_GDEY037T03::powerOff(void)
{
  _PowerOff();
}

void GxEPD2_4G_370_GDEY037T03::hibernate()
{
  _PowerOff();
  if (_rst >= 0)
  {
    _writeCommand(0x07); // deep sleep
    _writeData(0xA5);    // check code
    _hibernating = true;
    _init_display_done = false;
  }
}

void GxEPD2_4G_370_GDEY037T03::_setPartialRamArea(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
  uint16_t xe = (x + w - 1) | 0x0007; // byte boundary inclusive (last byte)
  uint16_t ye = y + h - 1;
  x &= 0xFFF8; // byte boundary
  _writeCommand(0x90); // partial window
  _writeData(x);
  _writeData(xe);
  _writeData(y / 256);
  _writeData(y % 256);
  _writeData(ye / 256);
  _writeData(ye % 256);
  _writeData(0x01);
}

void GxEPD2_4G_370_GDEY037T03::_PowerOn()
{
  if (!_power_is_on)
  {
    _writeCommand(0x04);
    _waitWhileBusy("_PowerOn", power_on_time);
  }
  _power_is_on = true;
}

void GxEPD2_4G_370_GDEY037T03::_PowerOff()
{
  if (_power_is_on)
  {
    _writeCommand(0x02); // power off
    _waitWhileBusy("_PowerOff", power_off_time);
  }
  _power_is_on = false;
}

void GxEPD2_4G_370_GDEY037T03::_InitDisplay()
{
  if (_hibernating) _reset();
  else
  {
    _writeCommand(0x00); // PANEL SETTING
    _writeData(0x1e);    // soft reset
    _writeData(0x0d);
    delay(1);
  }
  _power_is_on = false;
  _writeCommand(0x00); // PANEL SETTING
  _writeData(0x1f);    // KW: 3f, KWR: 2F, BWROTP: 0f, BWOTP: 1f
  _writeData(0x0d);
  _init_display_done = true;
}

void GxEPD2_4G_370_GDEY037T03::_Update_Full()
{
  if (useFastFullUpdate)
  {
    _writeCommand(0xE0); // Cascade Setting (CCSET)
    _writeData(0x02);    // TSFIX
    _writeCommand(0xE5); // Force Temperature (TSSET)
    _writeData(0x5A);    // 1005000us, less greyish
  }
  _writeCommand(0x50);
  // Dynamically write CDI setting depending on mode: 0x87 for true grayscale, 0x97 for standard monochrome
  _writeData(_grayscale_mode ? 0x87 : 0x97);
  _PowerOn();
  _writeCommand(0x12); //display refresh
  _waitWhileBusy("_Update_Full", full_refresh_time);
  _PowerOff();
  if (useFastFullUpdate) _InitDisplay(); // undo TSFIX
}

void GxEPD2_4G_370_GDEY037T03::_Update_Part()
{
  if (hasFastPartialUpdate)
  {
    _writeCommand(0xE0); // Cascade Setting (CCSET)
    _writeData(0x02);    // TSFIX
    _writeCommand(0xE5); // Force Temperature (TSSET)
    _writeData(0x6E);
  }
  _writeCommand(0x50);
  // Always write 0xD7 (monochrome partial update) to register 0x50 to completely prevent smearing
  _writeData(0xD7);
  _PowerOn();
  _writeCommand(0x12); //display refresh
  _waitWhileBusy("_Update_Part", partial_refresh_time);
  _PowerOff();
  if (hasFastPartialUpdate) _InitDisplay(); // undo TSFIX
}

void GxEPD2_4G_370_GDEY037T03::writeImage_4G(const uint8_t bitmap[], uint8_t bpp, int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  if (bpp != 2) return;
  _grayscale_mode = true;
  
  delay(1);
  uint16_t wb = (w + 3) / 4; // width bytes of 2bpp bitmap
  x -= x % 8;
  w = ((w + 7) / 8) * 8;
  int16_t x1 = x < 0 ? 0 : x;
  int16_t y1 = y < 0 ? 0 : y;
  int16_t w1 = x + w < int16_t(WIDTH) ? w : int16_t(WIDTH) - x;
  int16_t h1 = y + h < int16_t(HEIGHT) ? h : int16_t(HEIGHT) - y;
  int16_t dx = x1 - x;
  int16_t dy = y1 - y;
  w1 -= dx;
  h1 -= dy;
  if ((w1 <= 0) || (h1 <= 0)) return;
  if (!_init_display_done) _InitDisplay();
  if (_initial_write) writeScreenBuffer();
  
  _writeCommand(0x91); // partial in
  _setPartialRamArea(x1, y1, w1, h1);
  
  // Write LSB plane to command 0x10.
  _writeCommand(0x10);
  _startTransfer();
  for (int16_t i = 0; i < h1; i++)
  {
    int16_t y_idx = mirror_y ? (h - 1 - (i + dy)) : (i + dy);
    for (int16_t j = 0; j < w1 / 8; j++)
    {
      uint16_t idx0 = (j * 2) + dx / 4 + y_idx * wb;
      uint16_t idx1 = idx0 + 1;
      uint8_t val0 = pgm ? pgm_read_byte(&bitmap[idx0]) : bitmap[idx0];
      uint8_t val1 = pgm ? pgm_read_byte(&bitmap[idx1]) : bitmap[idx1];
      if (invert) {
        val0 = ~val0;
        val1 = ~val1;
      }
      uint8_t lsb0 = ((val0 >> 3) & 8) | ((val0 >> 2) & 4) | ((val0 >> 1) & 2) | (val0 & 1);
      uint8_t lsb1 = ((val1 >> 3) & 8) | ((val1 >> 2) & 4) | ((val1 >> 1) & 2) | (val1 & 1);
      uint8_t lsb_byte = (lsb0 << 4) | lsb1;
      _transfer(lsb_byte);
    }
  }
  _endTransfer();

  // Write MSB plane to command 0x13.
  _writeCommand(0x13);
  _startTransfer();
  for (int16_t i = 0; i < h1; i++)
  {
    int16_t y_idx = mirror_y ? (h - 1 - (i + dy)) : (i + dy);
    for (int16_t j = 0; j < w1 / 8; j++)
    {
      uint16_t idx0 = (j * 2) + dx / 4 + y_idx * wb;
      uint16_t idx1 = idx0 + 1;
      uint8_t val0 = pgm ? pgm_read_byte(&bitmap[idx0]) : bitmap[idx0];
      uint8_t val1 = pgm ? pgm_read_byte(&bitmap[idx1]) : bitmap[idx1];
      if (invert) {
        val0 = ~val0;
        val1 = ~val1;
      }
      uint8_t msb0 = ((val0 >> 4) & 8) | ((val0 >> 3) & 4) | ((val0 >> 2) & 2) | ((val0 >> 1) & 1);
      uint8_t msb1 = ((val1 >> 4) & 8) | ((val1 >> 3) & 4) | ((val1 >> 2) & 2) | ((val1 >> 1) & 1);
      uint8_t msb_byte = (msb0 << 4) | msb1;
      _transfer(msb_byte);
    }
  }
  _endTransfer();
  _writeCommand(0x92); // partial out
  delay(1);
}

void GxEPD2_4G_370_GDEY037T03::writeImagePart_4G(const uint8_t bitmap[], uint8_t bpp, int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                                             int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  if (bpp != 2) return;
  _grayscale_mode = true;

  delay(1);
  if ((w_bitmap < 0) || (h_bitmap < 0) || (w < 0) || (h < 0)) return;
  if ((x_part < 0) || (x_part >= w_bitmap)) return;
  if ((y_part < 0) || (y_part >= h_bitmap)) return;
  uint16_t wb_bitmap = (w_bitmap + 3) / 4; // width bytes of 2bpp bitmap
  x_part -= x_part % 8;
  w = w_bitmap - x_part < w ? w_bitmap - x_part : w;
  h = h_bitmap - y_part < h ? h_bitmap - y_part : h;
  x -= x % 8;
  w = ((w + 7) / 8) * 8;
  int16_t x1 = x < 0 ? 0 : x;
  int16_t y1 = y < 0 ? 0 : y;
  int16_t w1 = x + w < int16_t(WIDTH) ? w : int16_t(WIDTH) - x;
  int16_t h1 = y + h < int16_t(HEIGHT) ? h : int16_t(HEIGHT) - y;
  int16_t dx = x1 - x;
  int16_t dy = y1 - y;
  w1 -= dx;
  h1 -= dy;
  if ((w1 <= 0) || (h1 <= 0)) return;
  if (!_init_display_done) _InitDisplay();
  if (_initial_write) writeScreenBuffer();
  _writeCommand(0x91); // partial in
  _setPartialRamArea(x1, y1, w1, h1);

  // LSB plane
  _writeCommand(0x10);
  _startTransfer();
  for (int16_t i = 0; i < h1; i++)
  {
    int16_t y_idx = mirror_y ? (h_bitmap - 1 - (y_part + i + dy)) : (y_part + i + dy);
    for (int16_t j = 0; j < w1 / 8; j++)
    {
      uint16_t idx0 = x_part / 4 + (j * 2) + dx / 4 + y_idx * wb_bitmap;
      uint16_t idx1 = idx0 + 1;
      uint8_t val0 = pgm ? pgm_read_byte(&bitmap[idx0]) : bitmap[idx0];
      uint8_t val1 = pgm ? pgm_read_byte(&bitmap[idx1]) : bitmap[idx1];
      if (invert) {
        val0 = ~val0;
        val1 = ~val1;
      }
      uint8_t lsb0 = ((val0 >> 3) & 8) | ((val0 >> 2) & 4) | ((val0 >> 1) & 2) | (val0 & 1);
      uint8_t lsb1 = ((val1 >> 3) & 8) | ((val1 >> 2) & 4) | ((val1 >> 1) & 2) | (val1 & 1);
      uint8_t lsb_byte = (lsb0 << 4) | lsb1;
      _transfer(lsb_byte);
    }
  }
  _endTransfer();

  // MSB plane
  _writeCommand(0x13);
  _startTransfer();
  for (int16_t i = 0; i < h1; i++)
  {
    int16_t y_idx = mirror_y ? (h_bitmap - 1 - (y_part + i + dy)) : (y_part + i + dy);
    for (int16_t j = 0; j < w1 / 8; j++)
    {
      uint16_t idx0 = x_part / 4 + (j * 2) + dx / 4 + y_idx * wb_bitmap;
      uint16_t idx1 = idx0 + 1;
      uint8_t val0 = pgm ? pgm_read_byte(&bitmap[idx0]) : bitmap[idx0];
      uint8_t val1 = pgm ? pgm_read_byte(&bitmap[idx1]) : bitmap[idx1];
      if (invert) {
        val0 = ~val0;
        val1 = ~val1;
      }
      uint8_t msb0 = ((val0 >> 4) & 8) | ((val0 >> 3) & 4) | ((val0 >> 2) & 2) | ((val0 >> 1) & 1);
      uint8_t msb1 = ((val1 >> 4) & 8) | ((val1 >> 3) & 4) | ((val1 >> 2) & 2) | ((val1 >> 1) & 1);
      uint8_t msb_byte = (msb0 << 4) | msb1;
      _transfer(msb_byte);
    }
  }
  _endTransfer();
  _writeCommand(0x92); // partial out
  delay(1);
}

void GxEPD2_4G_370_GDEY037T03::drawImage_4G(const uint8_t bitmap[], uint8_t bpp, int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  writeImage_4G(bitmap, bpp, x, y, w, h, invert, mirror_y, pgm);
  refresh(x, y, w, h);
}

void GxEPD2_4G_370_GDEY037T03::drawImagePart_4G(const uint8_t bitmap[], uint8_t bpp, int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                                            int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  writeImagePart_4G(bitmap, bpp, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
  refresh(x, y, w, h);
}