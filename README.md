# nRF e-Paper Book Reader (Zephyr RTOS & nRF Connect SDK Port)

This project contains the native **Zephyr OS / nRF Connect SDK** firmware for the nice!nano (nRF52840 ProMicro compatible) e-Paper Book Reader.

---

## 🛠️ Project Structure

```
nrf_epd/
├── CMakeLists.txt      # CMake project definition
├── Kconfig             # Custom Kconfig definitions
├── prj.conf            # Zephyr & Kconfig driver options
├── app.overlay         # Devicetree pin mapping (EPD, SD, Buttons, SAADC)
├── west.yml            # West workspace manifest file
├── drivers/            # Out-of-tree Zephyr drivers (GDEY037T03 EPD)
├── dts/                # Devicetree bindings
├── tools/              # Font converter & USB CDC transfer scripts
└── src/
    ├── config.h        # Global system macros & configuration
    ├── display.h/c     # EPD graphics engine & UI rendering
    ├── epub.h/c        # Streaming EPUB/ZIP parser & cover extractor
    ├── puff.h/c        # Lightweight Deflate decompression
    ├── font_extended.* # SD card binary font (.fnt) loader
    ├── storage.h/c     # FatFS (SD card /SD:) file system driver
    ├── buttons.h/c     # GPIO interrupts, gesture debouncer, deep sleep wake
    ├── ble.h/c         # Zephyr Bluetooth stack (Nordic UART Service, BAS)
    ├── battery.h/c     # SAADC battery voltage telemetry
    └── main.c          # Reader state machine, menu system, auto-sleep
```

---

## 🚀 Building the Project

### Prerequisites
- [nRF Connect SDK v2.x+](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/nrf/installation.html)
- `west` toolchain

### Step-by-step Build Commands

1. Open your terminal initialized with the nRF Connect SDK environment.
2. Navigate to the repository root:
   ```bash
   cd c:\Users\Scott\source\repos\nrf_epd
   ```
3. Run `west build` specifying the target board:
   ```bash
   west build -b promicro_nrf52840/nrf52840/uf2 --pristine
   ```
   *(Alternative board target: `west build -b nice_nano_v2 --pristine`)*

---

## ⚡ Flashing the Firmware

### Option 1: Drag & Drop via Adafruit UF2 Bootloader (Recommended)
1. Double-click the **SELECT button** (or double-tap Reset if accessible).
2. The board will reboot into bootloader mode and mount as a USB storage drive named `NICENANO`.
3. Copy the output binary file `build/zephyr/zephyr.uf2` directly onto the `NICENANO` drive:
   ```powershell
   Copy-Item .\build\zephyr\zephyr.uf2 D:\
   ```

### Option 2: Flash via J-Link / nrfjprog
If using an external SWD programmer (J-Link / nRF52 DK):
```bash
west flash
```

---

## 🎮 Button Controls & Gestures

| Gesture | Action |
|---|---|
| **PREV Button** | Page Backward / Navigate Up in Menus |
| **NEXT Button** | Page Forward / Navigate Down in Menus |
| **SELECT Single-Click** | Select Item / Open Menu / Toggle Screen |
| **SELECT Double-Click** | Reboot into **Adafruit UF2 Bootloader** (`NICENANO` USB Drive) |
| **SELECT 5s Long Press** | Enter **System OFF Deep Sleep** (< 2 µA power draw) |
