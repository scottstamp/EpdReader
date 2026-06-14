/*
  Copyright (c) 2014-2015 Arduino LLC.  All right reserved.
  Copyright (c) 2016 Sandeep Mistry All right reserved.
  Copyright (c) 2018, Adafruit Industries (adafruit.com)

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
  See the GNU Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "variant.h"
#include "wiring_constants.h"
#include "wiring_digital.h"
#include "nrf.h"

const uint32_t g_ADigitalPinMap[] =
{
  // D0 .. D13
  25,  // D0  is P0.25 (UART TX)
  24,  // D1  is P0.24 (UART RX)
  10,  // D2  is P0.10 (NFC2)
  47,  // D3  is P1.15 (LED1)
  42,  // D4  is P1.10 (LED2)
  40,  // D5  is P1.08
   7,  // D6  is P0.07
  34,  // D7  is P1.02 (Button)
  16,  // D8  is P0.16 (NeoPixel)
  26,  // D9  is P0.26
  27,  // D10 is P0.27
   6,  // D11 is P0.06
   8,  // D12 is P0.08
  41,  // D13 is P1.09

  // D14 .. D21 (aka A0 .. A7)
   4,  // D14 is P0.04 (A0)
   5,  // D15 is P0.05 (A1)
  30,  // D16 is P0.30 (A2)
  28,  // D17 is P0.28 (A3)
   2,  // D18 is P0.02 (A4)
   3,  // D19 is P0.03 (A5)
  29,  // D20 is P0.29 (A6, Battery)
  31,  // D21 is P0.31 (A7, ARef)

  // D22 .. D23 (aka I2C pins)
  12,  // D22 is P0.12 (SDA)
  11,  // D23 is P0.11 (SCL)

  // D24 .. D26 (aka SPI pins)
  15,  // D24 is P0.15 (SPI MISO)
  13,  // D25 is P0.13 (SPI MOSI)
  14,  // D26 is P0.14 (SPI SCK)

  // QSPI pins (not exposed via any header / test point)
  19,  // D27 is P0.19 (QSPI CLK)
  20,  // D28 is P0.20 (QSPI CS)
  17,  // D29 is P0.17 (QSPI Data 0)
  22,  // D30 is P0.22 (QSPI Data 1)
  23,  // D31 is P0.23 (QSPI Data 2)
  21,  // D32 is P0.21 (QSPI Data 3)

  // The remaining NFC pin
   9,  // D33 is P0.09 (NFC1, exposed only via test point on bottom of board)

  32+14, // D34 is P1.14
  32+13  // D35 is P1.13 (nice!nano physical pin D8 on v2)
};

void initVariant()
{
  // LED1 & LED2
  pinMode(PIN_LED1, OUTPUT);
  ledOff(PIN_LED1);

  pinMode(PIN_LED2, OUTPUT);
  ledOff(PIN_LED2);

  pinMode(PIN_NEOPIXEL_POWER, OUTPUT);
  digitalWrite(PIN_NEOPIXEL_POWER, HIGH);
}
