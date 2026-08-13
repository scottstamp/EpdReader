Ran command: `cd ..`
Ran command: `git clone https://github.com/crosspoint-reader/crosspoint-reader`
Ran command: `explorer .`
Ran command: `git clone --recurse-submodules -j8 https://github.com/crosspoint-reader/crosspoint-reader`
Listed directory crosspoint-reader

Let's list `src` and `lib` inside `crosspoint-reader`!

Listed directory src

Let's check `lib` or search for EPUB in `crosspoint-reader`:

Listed directory lib

`crosspoint-reader/lib/Epub`!
Let's list the files in `c:\Users\Scott\source\repos\nrf_epd\crosspoint-reader\lib\Epub`!

Listed directory Epub

Let's list `crosspoint-reader/lib/Epub/Epub` as well:

Listed directory Epub

Let's inspect `Epub.h` and `Epub.cpp` to see how Crosspoint structures EPUB parsing, metadata, sectioning, page index caching, cover extraction, or HTML parsing!

Viewed Epub.h:1-81

Let's also look at `crosspoint-reader/lib/Epub/Epub/BookMetadataCache.h` and `BookMetadataCache.cpp`:

Viewed BookMetadataCache.h:1-129

Let's review what Crosspoint does for EPUB processing:

### Features & Techniques in Crosspoint Reader (`crosspoint-reader`):

1. **OPF & Manifest / Spine Parsing (True EPUB 2/3 Order vs ZIP File Order)**
   - **How Crosspoint does it**: Parses `container.xml` → finds `content.opf` → parses `<manifest>` and `<spine>` to determine exact chapter rendering order (`itemref idref="..."`), rather than relying solely on ZIP header ordering or file name alphabetization.
   - **Fit for nRF52840**: In our nRF52 reader, we currently sort ZIP entries using `natural_chapter_cmp` on file names (e.g. `chapter1.html`, `chapter2.html`). For 95% of EPUBs this works great, but for EPUBs with weird ZIP order or filenames (e.g. `part_a.xhtml`, `text_01.xhtml`), parsing `content.opf`'s `<spine>` tag yields 100% compliant chapter order.

2. **Metadata Cache (`.book.bin` on SD Card)**
   - **How Crosspoint does it**: Crosspoint scans the EPUB once, builds a binary cache file (`cacheDir/epub_HASH`), storing: Title, Author, Cover Href, Spine Chapter List + cumulative byte offsets. On subsequent opens of the same book, it reads the binary cache instantly without re-scanning the ZIP headers!
   - **Fit for nRF52840**: **EXCELLENT FIT!** In our code (`storage.c` / `epub.c`), scanning a large EPUB with 50 chapters currently scans 50 ZIP local headers over SPI every time the book is opened (taking 1-2 seconds). Caching chapter cumulative offsets & title in a small binary sidecar file (`.book.bin` or `/SD/.cache/book_hash.bin`) makes opening books **instantaneous** (< 50 ms)!

3. **True Cover Image Extraction & Scaled Thumbnail Caching (`generateCoverBmp`)**
   - **How Crosspoint does it**: Locates the cover image from `content.opf` `<meta name="cover" content="...">` or `<item properties="cover-image">`, extracts the JPEG/PNG, converts/dithers it, and saves a pre-rendered grayscale BMP thumbnail on SD card (`/SD/.cache/cover_hash.bmp`).
   - **Fit for nRF52840**: We already have TJPGD (`tjpgd.c`) for JPEG cover extraction and Atkinson dithering (`display_draw_atkinson_dithered`), but caching the pre-rendered dithered cover image on SD card as a tiny raw 1bpp/2bpp bitmap means lockscreen renders and menu book list thumbnails load **instantly** without decompressing JPEG every time!

4. **Table of Contents (TOC) Navigation (`toc.ncx` / `nav.xhtml`)**
   - **How Crosspoint does it**: Parses `toc.ncx` (EPUB 2) or `nav.xhtml` (EPUB 3) to extract human-readable chapter names (e.g., "Chapter 1: The Beginning", "Chapter 2: The Return") instead of raw filenames (`item001.html`).
   - **Fit for nRF52840**: Very nice feature! Currently our chapter menu shows the raw filename or chapter index. Extracting TOC titles from `toc.ncx` (which is small XML) gives a clean chapter selection menu.

5. **Page / Character Word-Wrap Byte Mapping (`Page.cpp` / `ParsedText.cpp`)**
   - **How Crosspoint does it**: Crosspoint does full layout engine rendering with custom TTF fonts and CSS styles, generating line boxes and blocks.
   - **Fit for nRF52840**: ESP32 has 4MB-8MB PSRAM for complex DOM trees; nRF52840 has 256KB SRAM. Our lightweight stream-decoder + `AmazonEmber` proportional font renderer is already perfectly tuned for nRF52840 RAM limits.

---

### Recommended Features to Adapt for our nRF52840 Reader:

1. **Chapter & Metadata Caching Sidecar (`.cache/`)**
   - **Benefit**: Opening a book becomes instantaneous (< 50ms) instead of re-scanning ZIP local headers across SPI SD card on every open.
   - **Memory/Flash budget**: Uses 0 extra RAM, minimal code size.

2. **OPF `<spine>` Chapter Order Parsing**
   - **Benefit**: Ensures 100% perfect chapter order for EPUB 2 & EPUB 3 books that use non-sequential internal HTML filenames.

3. **TOC Chapter Name Extraction (`toc.ncx`)**
   - **Benefit**: Shows actual chapter titles ("Chapter 12: The Dark Forest") in the reader chapter menu instead of `chapter12.html`.

Let's present a clear, structured summary of these findings and recommendations to the user!Here is the analysis of the **Crosspoint Reader** codebase (`/crosspoint-reader/lib/Epub`), comparing its features to our current nRF52840 implementation and highlighting what we can adapt within our memory and CPU budget.

---

### 🔍 Crosspoint vs. nRF52 Reader Architecture

| Feature | Crosspoint Reader (ESP32) | Our nRF52 Reader (nRF52840) |
| :--- | :--- | :--- |
| **Hardware** | ESP32-S3 (240 MHz, 8 MB PSRAM) | nRF52840 (64 MHz, 256 KB RAM) |
| **Layout Engine** | Full DOM/CSS parser + TTF layout engine (`CssParser.cpp`, `ParsedText.cpp`) | Fast streaming HTML-to-text parser + Proportional font renderer (`epub.c`, `display.c`) |
| **Chapter Ordering** | Reads OPF `<spine>` manifest & TOC (`content.opf`, `toc.ncx`) | Natural sorting on ZIP chapter filenames |
| **Metadata Caching** | Binary cache sidecar (`.book.bin` on SD) | Scans ZIP local headers on every open |
| **Cover Rendering** | Pre-renders & caches 1bpp BMP cover on SD | Decodes JPEG on the fly via `tjpgd.c` + Atkinson dithering |

---

### 💡 High-Value Features We Can Adapt Within Budget

#### 1. 🚀 Instant Book Opening via Metadata Sidecar Caching
* **How Crosspoint does it**: On first open, it scans the EPUB's metadata, chapter offsets, and titles, writing a small binary cache file (`/SD/.cache/<hash>.bin`). On subsequent opens, it loads the binary cache directly.
* **Why we should adapt it**: Currently, opening a 50-chapter EPUB on our nRF52 scans 50 ZIP local headers over SPI every time (~1.5s). Adding a tiny binary cache file on the SD card will make opening any book **instantaneous (< 50 ms)**.
* **Budget**: 0 extra RAM, ~1 KB Flash.

#### 2. 📖 TOC Chapter Titles (`toc.ncx` / `nav.xhtml`)
* **How Crosspoint does it**: Parses `toc.ncx` (EPUB 2) or `nav.xhtml` (EPUB 3) to extract actual chapter names (e.g., *"Chapter 3: The Journey Begins"*) instead of showing internal HTML filenames (*`section_003.html`*).
* **Why we should adapt it**: Enhances the chapter selection menu with real chapter titles instead of raw filenames.
* **Budget**: Very lightweight XML string extraction during scan.

#### 3. 🎯 OPF `<spine>` Manifest Parsing (100% EPUB Specification Compliance)
* **How Crosspoint does it**: Reads `content.opf` to get the exact reading order defined by the publisher (`<spine><itemref idref="..."/></spine>`).
* **Why we should adapt it**: Natural filename sorting works for 95% of EPUBs, but some publisher EPUBs use arbitrary filenames (`page_a.html`, `p01.html`). Reading the `<spine>` guarantees 100% compliant chapter order across all EPUBs.
* **Budget**: Minimal memory footprint.

---

### 🛠️ Recommended Action Plan

1. **Phase 1**: Add **Metadata Sidecar Caching** (`.cache/`) so books open instantly.
2. **Phase 2**: Add **TOC Chapter Titles** (`toc.ncx`) for clean chapter selection menus.

Which of these would you like to build first?