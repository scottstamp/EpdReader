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

## ✅ CI & Host Tests

GitHub Actions (`.github/workflows/ci.yml`) runs on every push and pull request:

- **host-tests** (ubuntu, gcc/python/node only, no hardware): the tests below, plus a check that
  `src/icons.h` matches `tools/icon_gen.py`. The UI preview PNGs are uploaded as the `ui-preview`
  artifact, so UI changes in a PR can be reviewed by eye.
- **firmware**: builds in Nordic's `ghcr.io/nrfconnect/sdk-nrf-toolchain:v3.4.1` container. A west
  workspace is created from `west.yml` (only the NCS modules the app uses, shallow clones). The job
  fails on any build warning, prints RAM/FLASH usage in the run summary and uploads `zephyr.uf2` /
  `zephyr.elf` as the `firmware` artifact.

Run the host tests locally from the repo root (needs `gcc`, `python`, `node` on PATH):

```bash
python tools/upload_test/test_upload.py        # BLE upload protocol (client vs simulated device)
mkdir -p /tmp/up && gcc -Wall -Wextra -Werror -std=c11 -pedantic -O2 -I src -o /tmp/up/sim \
    tools/upload_test/upload_sim.c src/upload_proto.c && /tmp/up/sim /tmp/up   # protocol unit tests
node tools/lists_web/test/test_web.mjs         # lists doc format + Web Bluetooth client
python tools/ui_preview/preview.py             # renders screens to tools/ui_preview/out/*.png
python tools/icon_gen.py && git diff --exit-code src/icons.h   # icons.h up to date
```

`west.yml` is only used to create a workspace from scratch (as CI does); building with an
installed NCS v3.4.1 is unaffected. If a new Kconfig option needs another NCS module (e.g.
littlefs, mbedtls), add it to the `name-allowlist` in `west.yml`.

---

## ⚡ Flashing the Firmware

### Option 1: Drag & Drop via Adafruit UF2 Bootloader (Recommended)
1. Choose **Bootloader Mode** in the main menu (or double-tap Reset if accessible).
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
| **SELECT Click** | Select item / open the reader menu (acts on release) |
| **SELECT Hold (1s)** | Back to the home carousel from the reader or any submenu; on the home screen, **System OFF Deep Sleep** (< 2 µA power draw) |
