# nRF e-Paper Book Reader

Zephyr / nRF Connect SDK firmware for a pocket e-paper reader built on a nice!nano
(nRF52840, ProMicro-compatible) and a Good Display GDEY037T03 3.7" panel (UC8253).
Wiring is in [WIRING.md](WIRING.md).

**Features**
- EPUB and plain-text books from the SD card. EPUBs are converted to text once on first open
  (streaming inflate, so no heap), keep chapter titles and italic/bold/underline, and resume
  where you left off.
- Lock screen with the book cover, dithered from the EPUB's JPEG.
- Built-in Bookerly and Amazon Ember fonts, or a font from the SD card (`.fnt`, UTF-8).
- To-do and grocery lists, edited on the device or from a browser over Bluetooth
  ([tools/lists_web](tools/lists_web/README.md)).
- Book transfer over USB (the SD card appears as a drive) or Bluetooth (`tools/upload.py --ble`).
- Tunable partial-refresh waveform, settings and reading stats kept in flash (NVS), System OFF
  sleep with the panel supply switched off.

---

## Project structure

```
nrf_epd/
├── CMakeLists.txt, Kconfig, prj.conf
├── app.overlay           # Devicetree: EPD, SD card, buttons, battery ADC, USB
├── west.yml              # West manifest (used by CI to create a workspace)
├── drivers/display/      # Out-of-tree GDEY037T03 (UC8253) driver
├── dts/bindings/         # Devicetree binding for the panel
├── fonts/                # Source fonts and convert_fonts.py
├── tools/                # Upload client, UI preview, icon generator, lists web page, tests
└── src/
    ├── main.c            # Init, restore the screen shown before sleep
    ├── app.c/h           # Screen state machine and button loop
    ├── screens.h         # Screen tables and shared screen helpers
    ├── screen_*.c        # One file per area: home, library, reader, settings, lists,
    │                     #   transfer (USB/BLE upload), sleep (lock screen)
    ├── book.c/h          # Open book, paging, progress saves, reading time
    ├── display.c/h       # Framebuffer, text layout, menus, carousel, lock screen
    ├── epub.c/h          # EPUB index/conversion, cover decoding
    ├── inflate_stream.*  # Streaming Deflate for EPUB entries
    ├── html_text.*       # Streaming XHTML to styled text
    ├── dither.c/h        # Tone-mapped Floyd-Steinberg dithering for covers
    ├── font_extended.*   # SD card font loader
    ├── storage.c/h       # SD card (FatFs): books, progress, cache, sleep state
    ├── prefs.c/h         # Settings and stats in NVS
    ├── lists*.c/h        # Lists document, storage and Bluetooth service
    ├── ble*.c/h, upload_proto.*  # Bluetooth stack, book upload protocol
    ├── buttons.c/h       # Debounced buttons, long press, wake from System OFF
    ├── battery.c/h       # Battery voltage (also reported over the BLE Battery Service)
    └── third_party/tjpgd # JPEG decoder for covers
```

How the UI is organised: every screen is a `Screen` table of hooks (`render`, `step`, `select`,
optional `enter`/`leave`/`poll`) registered in `app.c`. `app_go()` runs the old screen's `leave`
and the new screen's `enter` before drawing, so anything a screen starts (Bluetooth sync, USB
drive mode, portrait layout) is released however you leave it, including auto-sleep.

### SD card layout

| Path | Contents |
|---|---|
| `/books` | `.epub` and `.txt` books (`/BOOKS` and `/Books` also work) |
| `/fonts` | Optional `.fnt` font for the "SD card" font setting |
| `/cache` | Converted text, indexes and covers (safe to delete; rebuilt on open) |
| `/lists.txt` | Lists document |
| `progress.dat`, `sleep.txt` | Reading positions, screen to restore after sleep |

---

## Building

Requires nRF Connect SDK **v3.4.1** (the nRF Connect for VS Code extension works, or `west`
from the NCS toolchain).

```bash
west build -b promicro_nrf52840/nrf52840/uf2 --sysbuild --pristine
```

The firmware image is `build/nrf_epd/zephyr/zephyr.uf2`.

`west.yml` is only for creating a workspace from scratch (as CI does); building with an
installed NCS is unaffected. If a new Kconfig option needs another NCS module (e.g. littlefs,
mbedtls), add it to the `name-allowlist` in `west.yml`.

## Flashing

**UF2 bootloader (no programmer):** choose **Settings > Firmware update (bootloader)** on the
device (or double-tap reset). It mounts as a USB drive named `NICENANO`; copy `zephyr.uf2`
onto it and it reboots into the new firmware. Settings, stats and the SD card are kept.

**SWD (J-Link / nRF52 DK):** `west flash`.

## Tests and tools

Host tests, run from the repo root (need `gcc`, `python`, `node` on PATH):

```bash
python tools/upload_test/test_upload.py        # BLE upload protocol (client vs simulated device)
mkdir -p /tmp/up && gcc -Wall -Wextra -Werror -std=c11 -pedantic -O2 -I src -o /tmp/up/sim \
    tools/upload_test/upload_sim.c src/upload_proto.c && /tmp/up/sim /tmp/up   # protocol unit tests
node tools/lists_web/test/test_web.mjs         # lists doc format + Web Bluetooth client
python tools/ui_preview/preview.py             # renders screens to tools/ui_preview/out/*.png
python tools/icon_gen.py && git diff --exit-code src/icons.h   # icons.h up to date
```

- `tools/ui_preview/preview.py` builds `display.c` for the PC and renders every screen to PNG,
  so layout changes can be checked without flashing (`--cover`, `--books` for real data).
- `tools/upload.py` sends a book over USB serial (`--port`) or Bluetooth (`--ble`). For
  Bluetooth, open **Transfer > Bluetooth upload** on the device first.

## Releases (CI)

GitHub Actions ([.github/workflows/ci.yml](.github/workflows/ci.yml)) runs only for pushed
tags starting with `v`, or when started by hand from the Actions tab. Day-to-day builds are
done locally.

1. Host tests, plus the UI preview PNGs as an artifact.
2. Firmware build in Nordic's `ghcr.io/nrfconnect/sdk-nrf-toolchain:v3.4.1` container. Fails
   on any build warning and reports RAM/FLASH usage.
3. For a tag, a GitHub release with `epd-reader-<tag>.uf2` and `.elf` attached and notes
   generated from the commits. Tags with a suffix (`v1.2.0-rc1`) are marked as pre-releases.

```bash
git tag v1.0.0 && git push origin v1.0.0
```

---

## Controls

| Button | Action |
|---|---|
| **Prev** | Previous page / move up in menus |
| **Next** | Next page / move down in menus |
| **Select** click | Choose the item / open the reader menu (acts on release) |
| **Select** hold (1 s) | Back to the home carousel from the reader or any menu. On the home screen, stats, or a transfer screen: lock screen and sleep |

The device also sleeps after 5 minutes without input (not while in USB drive mode or with a
Bluetooth client connected). Any button wakes it to the screen it was on.
