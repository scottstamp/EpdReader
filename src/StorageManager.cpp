#include "StorageManager.h"
#include "Config.h"

#include "DisplayManager.h"
#include "ButtonManager.h"
#include <SPI.h>
#include <Adafruit_TinyUSB.h>

// Instantiate dedicated hardware SPI port for the SD Card
SPIClass SPI2(NRF_SPIM2, SD_MISO, SD_SCK, SD_MOSI);

Adafruit_USBD_MSC usb_msc;

// Callbacks for MSC
int32_t msc_read_cb(uint32_t lba, void* buffer, uint32_t bufsize) {
    bool ok = StorageManager::getInstance().sdCardReadSectors(lba, buffer, bufsize);
    return ok ? (int32_t)bufsize : -1;
}

int32_t msc_write_cb(uint32_t lba, uint8_t* buffer, uint32_t bufsize) {
    bool ok = StorageManager::getInstance().sdCardWriteSectors(lba, buffer, bufsize);
    return ok ? (int32_t)bufsize : -1;
}

void msc_flush_cb(void) {
    // No-op
}

using namespace Adafruit_LittleFS_Namespace;
using LfsFile = Adafruit_LittleFS_Namespace::File;

static String getBookmarkFilename(const String& filename);

StorageManager& StorageManager::getInstance() {
    static StorageManager instance;
    return instance;
}

StorageManager::StorageManager() : _isUploading(false), _sdInitialized(false), _statsLoaded(false), _cachedTotalSeconds(0) {}

bool StorageManager::begin() {
    // Initialize USB Mass Storage interface
    usb_msc.setID("nice!nano", "SD Reader", "1.0");
    usb_msc.setReadWriteCallback(msc_read_cb, msc_write_cb, msc_flush_cb);
    usb_msc.setUnitReady(false); // Media is not present/ready initially
    usb_msc.begin();

    // Start InternalFS (LittleFS)
    if (!InternalFS.begin()) {
        // If mounting fails, format
        Serial.println("LittleFS mount failed! Formatting...");
        if (!InternalFS.format()) {
            Serial.println("Format failed!");
            return false;
        }
        if (!InternalFS.begin()) {
            Serial.println("Retry mount failed!");
            return false;
        }
    }
    
    Serial.println("LittleFS mounted successfully.");
    createBookDir();

    // Diagnostic print: list all files on the device on boot
    Serial.println("--- Current Files on Flash ---");
    LfsFile dir = InternalFS.open(BOOK_DIR, FILE_O_READ);
    if (dir && dir.isDirectory()) {
        LfsFile file(InternalFS);
        uint32_t totalUsed = 0;
        int fileCount = 0;
        while ((file = dir.openNextFile())) {
            Serial.print("  - ");
            Serial.print(file.name());
            Serial.print(" (");
            Serial.print(file.size());
            Serial.println(" bytes)");
            totalUsed += file.size();
            fileCount++;
            file.close();
        }
        dir.close();
        Serial.print("Total files: ");
        Serial.print(fileCount);
        Serial.print(", Total used size: ");
        Serial.print(totalUsed);
        Serial.print(" / ");
        Serial.print(TOTAL_FS_SIZE);
        Serial.println(" bytes.");
    } else {
        Serial.println("Failed to open books directory or it is empty.");
    }
    Serial.println("-----------------------------");

    return true;
}

bool StorageManager::createBookDir() {
    if (!InternalFS.exists(BOOK_DIR)) {
        Serial.println("Books directory does not exist. Creating...");
        if (!InternalFS.mkdir(BOOK_DIR)) {
            Serial.println("Failed to create books directory!");
            return false;
        }
    }
    return true;
}

int StorageManager::listBooks(String books[], int maxBooks) {
    int count = 0;
    LfsFile dir = InternalFS.open(BOOK_DIR, FILE_O_READ);
    if (!dir) {
        Serial.println("Failed to open books directory!");
        return 0;
    }
    
    if (!dir.isDirectory()) {
        Serial.println("Books path is not a directory!");
        return 0;
    }

    LfsFile file(InternalFS);
    while (count < maxBooks && (file = dir.openNextFile())) {
        String name = file.name();
        // Skip temp.txt or dotfiles
        if (name != "temp.txt" && !name.startsWith(".")) {
            books[count++] = name;
        }
        file.close();
    }
    dir.close();
    return count;
}

bool StorageManager::deleteBook(const String& filename) {
    if (!beginSD()) return false;
    String fullPath = "/" + filename;
    String bmkPath = "/bookmarks/" + getBookmarkFilename(filename) + ".bmk";
    if (sd.exists(bmkPath.c_str())) {
        sd.remove(bmkPath.c_str());
    }
    if (sd.exists(fullPath.c_str())) {
        return sd.remove(fullPath.c_str());
    }
    return false;
}

uint32_t StorageManager::getUsedSpace() {
    if (!beginSD()) return 0;
    uint32_t used = 0;
    FsFile root = sd.open("/", O_RDONLY);
    if (!root) return 0;
    
    FsFile file;
    while (file.openNext(&root, O_RDONLY)) {
        char name[100];
        file.getName(name, sizeof(name));
        String filename = String(name);
        if (filename != "temp.txt" && !filename.startsWith(".") && filename.endsWith(".txt")) {
            used += file.size();
        }
        file.close();
    }
    root.close();
    return used;
}

bool StorageManager::writeProgress(const String& currentBook, uint32_t offset) {
    if (!beginSD()) {
        Serial.println("Failed to init SD card for writing progress.");
        return false;
    }

    if (sd.exists(PROGRESS_FILE)) {
        sd.remove(PROGRESS_FILE);
    }

    FsFile progressFile = sd.open(PROGRESS_FILE, O_WRONLY | O_CREAT | O_TRUNC);
    if (!progressFile) {
        Serial.println("Failed to open progress file on SD for writing.");
        return false;
    }
    
    progressFile.println(currentBook);
    progressFile.println(offset);

    // Save history offsets
    uint32_t hist[HISTORY_SIZE];
    int count = DisplayManager::getInstance().getHistoryOffsets(hist, HISTORY_SIZE);
    progressFile.println(count);
    for (int i = 0; i < count; i++) {
        progressFile.println(hist[i]);
    }

    progressFile.close();
    return true;
}

bool StorageManager::readProgress(String& currentBook, uint32_t& offset) {
    if (!beginSD() || !sd.exists(PROGRESS_FILE)) {
        currentBook = "";
        offset = 0;
        return false;
    }

    FsFile progressFile = sd.open(PROGRESS_FILE, O_RDONLY);
    if (!progressFile) {
        return false;
    }

    currentBook = progressFile.readStringUntil('\n');
    currentBook.trim();
    
    String offsetStr = progressFile.readStringUntil('\n');
    offsetStr.trim();
    offset = offsetStr.toInt();
    
    progressFile.close();
    return true;
}

bool StorageManager::readProgress(String& currentBook, uint32_t& offset, uint32_t* historyDest, int maxHistoryLen, int& historyCount) {
    if (!beginSD() || !sd.exists(PROGRESS_FILE)) {
        currentBook = "";
        offset = 0;
        historyCount = 0;
        return false;
    }

    FsFile progressFile = sd.open(PROGRESS_FILE, O_RDONLY);
    if (!progressFile) {
        historyCount = 0;
        return false;
    }

    currentBook = progressFile.readStringUntil('\n');
    currentBook.trim();
    
    String offsetStr = progressFile.readStringUntil('\n');
    offsetStr.trim();
    offset = offsetStr.toInt();
    
    historyCount = 0;
    if (progressFile.available()) {
        String countStr = progressFile.readStringUntil('\n');
        countStr.trim();
        int count = countStr.toInt();
        for (int i = 0; i < count; i++) {
            if (progressFile.available()) {
                String valStr = progressFile.readStringUntil('\n');
                valStr.trim();
                if (i < maxHistoryLen) {
                    historyDest[historyCount++] = valStr.toInt();
                }
            }
        }
    }
    
    progressFile.close();
    return true;
}

static String getBookmarkFilename(const String& filename) {
    String cleanName = filename;
    if (cleanName.startsWith("[SD]")) cleanName = cleanName.substring(4);
    else if (cleanName.startsWith("[BLE]")) cleanName = cleanName.substring(5);
    
    int slashIdx = cleanName.indexOf('/');
    if (slashIdx > 0) {
        cleanName = cleanName.substring(0, slashIdx);
    }
    cleanName.replace("/", "_");
    return cleanName;
}

bool StorageManager::writeBookmark(const String& filename, uint32_t offset) {
    if (!beginSD()) {
        return false;
    }
    if (!sd.exists("/bookmarks")) {
        if (!sd.mkdir("/bookmarks")) {
            Serial.println("Failed to create bookmarks directory on SD.");
            return false;
        }
    }
    String path = "/bookmarks/" + getBookmarkFilename(filename) + ".bmk";
    if (sd.exists(path.c_str())) {
        sd.remove(path.c_str());
    }

    FsFile progressFile = sd.open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
    if (!progressFile) {
        Serial.println("Failed to open bookmark file for writing.");
        return false;
    }
    progressFile.println(offset);

    // Save history offsets
    uint32_t hist[HISTORY_SIZE];
    int count = DisplayManager::getInstance().getHistoryOffsets(hist, HISTORY_SIZE);
    progressFile.println(count);
    for (int i = 0; i < count; i++) {
        progressFile.println(hist[i]);
    }

    progressFile.close();
    return true;
}

bool StorageManager::readBookmark(const String& filename, uint32_t& offset) {
    if (!beginSD()) return false;
    String path = "/bookmarks/" + getBookmarkFilename(filename) + ".bmk";
    if (!sd.exists(path.c_str())) {
        offset = 0;
        return false;
    }
    FsFile progressFile = sd.open(path.c_str(), O_RDONLY);
    if (!progressFile) {
        return false;
    }
    String offsetStr = progressFile.readStringUntil('\n');
    offsetStr.trim();
    offset = offsetStr.toInt();
    progressFile.close();
    return true;
}

bool StorageManager::readBookmark(const String& filename, uint32_t& offset, uint32_t* historyDest, int maxHistoryLen, int& historyCount) {
    if (!beginSD()) {
        offset = 0;
        historyCount = 0;
        return false;
    }
    String path = "/bookmarks/" + getBookmarkFilename(filename) + ".bmk";
    if (!sd.exists(path.c_str())) {
        offset = 0;
        historyCount = 0;
        return false;
    }
    FsFile progressFile = sd.open(path.c_str(), O_RDONLY);
    if (!progressFile) {
        historyCount = 0;
        return false;
    }
    String offsetStr = progressFile.readStringUntil('\n');
    offsetStr.trim();
    offset = offsetStr.toInt();
    
    historyCount = 0;
    if (progressFile.available()) {
        String countStr = progressFile.readStringUntil('\n');
        countStr.trim();
        int count = countStr.toInt();
        for (int i = 0; i < count; i++) {
            if (progressFile.available()) {
                String valStr = progressFile.readStringUntil('\n');
                valStr.trim();
                if (i < maxHistoryLen) {
                    historyDest[historyCount++] = valStr.toInt();
                }
            }
        }
    }
    
    progressFile.close();
    return true;
}

Adafruit_LittleFS_Namespace::File StorageManager::openBook(const String& filename, const char* mode) {
    String fullPath = String(BOOK_DIR) + "/" + filename;
    uint8_t flags = FILE_O_READ;
    if (strcmp(mode, "w") == 0) {
        flags = FILE_O_WRITE;
    }
    return InternalFS.open(fullPath.c_str(), flags);
}

bool StorageManager::startNewBookWrite() {
    if (!beginSD()) {
        return false;
    }

    if (_isUploading && _uploadFile) {
        _uploadFile.close();
    }
    
    // Always write to temp.txt first to ensure atomic updates
    if (sd.exists("/temp.txt")) {
        sd.remove("/temp.txt");
    }

    _uploadFile = sd.open("/temp.txt", O_WRONLY | O_CREAT | O_TRUNC);
    if (!_uploadFile) {
        Serial.println("Failed to open temp.txt on SD for writing BLE stream.");
        _isUploading = false;
        return false;
    }

    _isUploading = true;
    Serial.println("Started writing to temp.txt on SD...");
    return true;
}

bool StorageManager::writeBookChunk(const uint8_t* data, size_t len) {
    if (!_isUploading || !_uploadFile) {
        Serial.println("[Device Debug] writeBookChunk: Not uploading or file not open.");
        return false;
    }
    size_t written = _uploadFile.write(data, len);
    if (written != len) {
        Serial.print("[Device Debug] writeBookChunk: SD write failed! Expected ");
        Serial.print(len);
        Serial.print(" bytes, wrote ");
        Serial.println(written);
        return false;
    }
    return true;
}

bool StorageManager::finalizeBookWrite(const String& title) {
    if (!_isUploading || !_uploadFile) {
        Serial.println("[Device Debug] finalizeBookWrite: Not uploading or file not open.");
        return false;
    }
    _uploadFile.close();
    _isUploading = false;

    uint32_t tempSize = 0;
    FsFile tempFile = sd.open("/temp.txt", O_RDONLY);
    if (tempFile) {
        tempSize = tempFile.size();
        Serial.print("[Device Debug] temp.txt size on SD: ");
        Serial.print(tempSize);
        Serial.println(" bytes.");
        tempFile.close();
    } else {
        Serial.println("[Device Debug] Warning: Failed to open temp.txt on SD to check size.");
    }

    // Open temp.txt, extract the first line as book title if title is empty
    String finalTitle = title;
    finalTitle.trim();

    if (finalTitle.length() == 0) {
        tempFile = sd.open("/temp.txt", O_RDONLY);
        if (tempFile) {
            finalTitle = tempFile.readStringUntil('\n');
            finalTitle.trim();
            tempFile.close();
        }
    }

    // Clean up title to form a safe filename
    if (finalTitle.length() == 0) {
        finalTitle = "Uploaded_Book";
    }

    String cleanTitle = "";
    for (size_t i = 0; i < finalTitle.length(); i++) {
        char c = finalTitle.charAt(i);
        if (isalnum(c) || c == '_' || c == '-' || c == ' ') {
            if (c == ' ') cleanTitle += '_';
            else cleanTitle += c;
        }
    }
    
    // Avoid double file extension
    if (cleanTitle.endsWith(".txt")) {
        cleanTitle = cleanTitle.substring(0, cleanTitle.length() - 4);
    }
    
    // Add extension
    cleanTitle += ".txt";

    String finalPath = "/" + cleanTitle;

    if (sd.exists(finalPath.c_str())) {
        sd.remove(finalPath.c_str());
    }

    Serial.print("Renaming temp.txt to: ");
    Serial.println(finalPath);

    if (tempSize == 0) {
        Serial.println("[Device Debug] Warning: Attempting to rename an empty 0-byte file!");
    }

    if (!sd.rename("/temp.txt", finalPath.c_str())) {
        Serial.println("Rename failed on SD!");
        return false;
    }

    return true;
}

bool StorageManager::clearStorage() {
    if (_isUploading && _uploadFile) {
        _uploadFile.close();
        _isUploading = false;
    }
    
    Serial.println("[Device Debug] Formatting InternalFS...");
    InternalFS.end();
    if (!InternalFS.format()) {
        Serial.println("Format failed!");
        return false;
    }
    if (!InternalFS.begin()) {
        Serial.println("Re-mount failed!");
        return false;
    }
    createBookDir();
    // /stats.dat lived on InternalFS; it has been wiped by the format.

    Serial.println("[Device Debug] Formatting progress/bookmarks on SD...");
    if (beginSD()) {
        if (sd.exists(PROGRESS_FILE)) {
            sd.remove(PROGRESS_FILE);
        }
        if (sd.exists("/bookmarks")) {
            // Delete all files in /bookmarks
            FsFile bmkDir = sd.open("/bookmarks", O_RDONLY);
            if (bmkDir && bmkDir.isDir()) {
                FsFile file;
                while (file.openNext(&bmkDir, O_RDONLY)) {
                    char name[100];
                    file.getName(name, sizeof(name));
                    file.close();
                    String filePath = "/bookmarks/" + String(name);
                    sd.remove(filePath.c_str());
                }
                bmkDir.close();
                sd.rmdir("/bookmarks");
            }
        }
        if (sd.exists("/settings.dat")) sd.remove("/settings.dat");
        if (sd.exists("/sleep_state.dat")) sd.remove("/sleep_state.dat");
        if (sd.exists("/stats.dat")) sd.remove("/stats.dat");
    }
    Serial.println("[Device Debug] Storage cleared successfully.");
    return true;
}

bool StorageManager::recoverSDSoftware() {
    Serial.println("[SD Debug] Attempting software-only SPI recovery on dedicated SPI2...");

    // 1. Ensure SD CS is LOW (asserted) so the card processes clocks to flush any pending read/write
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, LOW);

    // 2. Ensure SPI2 is enabled
    SPI2.begin();

    // 3. Enable internal pull-up on SD_MISO pin to prevent floating
    pinMode(SD_MISO, INPUT_PULLUP);

    // 4. Send up to 600 bytes of 0xFF with CS LOW on SPI2.
    SPI2.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    bool misoReleased = false;
    for (int i = 0; i < 600; i++) {
        uint8_t res = SPI2.transfer(0xFF);
        // If MISO is 0xFF, it means the card has released the bus (MISO is HIGH).
        if (res == 0xFF) {
            // Verify MISO remains HIGH for a few more transfers to ensure stable release
            misoReleased = true;
            for (int j = 0; j < 5; j++) {
                if (SPI2.transfer(0xFF) != 0xFF) {
                    misoReleased = false;
                    break;
                }
            }
            if (misoReleased) {
                Serial.print("[SD Debug] Software recovery: MISO released after ");
                Serial.print(i);
                Serial.println(" bytes of active clocking on SPI2.");
                break;
            }
        }
    }
    SPI2.endTransaction();

    // 5. Deassert SD CS (HIGH)
    digitalWrite(SD_CS, HIGH);

    // 6. Send 80 clocks (10 bytes of 0xFF) with CS HIGH to reset the card's SPI receiver state machine
    SPI2.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    for (int i = 0; i < 10; i++) {
        SPI2.transfer(0xFF);
    }
    SPI2.endTransaction();

    // 7. Wait 5ms for card's internal controller to stabilize
    delay(5);

    // 8. Verify MISO is released (HIGH due to pull-up)
    if (digitalRead(SD_MISO) == LOW) {
        Serial.println("[SD Debug] Software recovery failed: MISO is still held LOW after flushing on SPI2!");
        return false;
    }

    Serial.println("[SD Debug] Software recovery: MISO is HIGH. Re-initializing card...");

    // 9. Attempt to begin SdFat at 4 MHz on dedicated SPI2
    if (sd.begin(SdSpiConfig(SD_CS, USER_SPI_BEGIN, SD_SCK_MHZ(4), &SPI2))) {
        _sdInitialized = true;
        Serial.println("[SD Debug] SD Card re-initialized successfully via software recovery at 4 MHz on dedicated SPI2.");
        return true;
    }

    // 10. Attempt at 2 MHz
    if (sd.begin(SdSpiConfig(SD_CS, USER_SPI_BEGIN, SD_SCK_MHZ(2), &SPI2))) {
        _sdInitialized = true;
        Serial.println("[SD Debug] SD Card re-initialized successfully via software recovery at 2 MHz on dedicated SPI2.");
        return true;
    }

    // 11. Attempt at 1 MHz
    if (sd.begin(SdSpiConfig(SD_CS, USER_SPI_BEGIN, SD_SCK_MHZ(1), &SPI2))) {
        _sdInitialized = true;
        Serial.println("[SD Debug] SD Card re-initialized successfully via software recovery at 1 MHz on dedicated SPI2.");
        return true;
    }

    Serial.println("[SD Debug] Software recovery failed to re-initialize card on dedicated SPI2.");
    return false;
}

void StorageManager::powerCyclePeripherals() {
    Serial.println("[SD Debug] Power-cycling peripherals (safely isolating pins to prevent latch-up)...");

    // 1. End SPI interfaces to release control of SPI pins
    SPI.end();
    SPI2.end();

    // 2. Set all peripheral pins to INPUT (tristate) so they do not parasitically power the chips
    pinMode(SD_CS, INPUT);
    pinMode(SD_SCK, INPUT);
    pinMode(SD_MOSI, INPUT);
    pinMode(SD_MISO, INPUT);
    
    pinMode(EPD_CS, INPUT);
    pinMode(EPD_DC, INPUT);
    pinMode(EPD_RST, INPUT);
    pinMode(EPD_MISO, INPUT);
    pinMode(EPD_MOSI, INPUT);
    pinMode(EPD_SCK, INPUT);
    pinMode(EPD_BUSY, INPUT);

    // 3. Power OFF VCC via pin 13
    ButtonManager::getInstance().setPeripheralPower(false);

    // 4. Wait 200ms for capacitors to discharge completely
    delay(200);

    // 5. Power ON VCC
    ButtonManager::getInstance().setPeripheralPower(true);

    // 6. Wait 50ms for voltage to stabilize
    delay(50);

    // 7. Configure pins back to safe default output states:
    // Drive CS and control pins HIGH (deasserted)
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);
    
    pinMode(EPD_CS, OUTPUT);
    digitalWrite(EPD_CS, HIGH);
    
    pinMode(EPD_DC, OUTPUT);
    digitalWrite(EPD_DC, HIGH);
    
    pinMode(EPD_RST, OUTPUT);
    digitalWrite(EPD_RST, HIGH);

    // Drive display clock and data lines LOW (idle state)
    pinMode(EPD_MOSI, OUTPUT);
    digitalWrite(EPD_MOSI, LOW);
    
    pinMode(EPD_SCK, OUTPUT);
    digitalWrite(EPD_SCK, LOW);

    // Drive SD clock and data lines LOW (idle state)
    pinMode(SD_MOSI, OUTPUT);
    digitalWrite(SD_MOSI, LOW);
    
    pinMode(SD_SCK, OUTPUT);
    digitalWrite(SD_SCK, LOW);

    // EPD_BUSY remains INPUT
    pinMode(EPD_BUSY, INPUT);

    // 8. Restore SPI and pin assignments
    SPI.setPins(EPD_MISO, EPD_SCK, EPD_MOSI);
    SPI.begin();
    pinMode(EPD_MISO, INPUT_PULLUP);

    SPI2.begin();
    pinMode(SD_MISO, INPUT_PULLUP);

    // 9. Send 16 dummy clock cycles with CS lines HIGH to clear SPI buses
    SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    SPI.transfer(0xFF);
    SPI.transfer(0xFF);
    SPI.endTransaction();

    SPI2.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    SPI2.transfer(0xFF);
    SPI2.transfer(0xFF);
    SPI2.endTransaction();

    // 10. Mark display as needing reinit since it lost power
    DisplayManager::getInstance().setDisplayNeedsReinit(true);
}

bool StorageManager::beginSD() {
    // Ensure display CS is deasserted (HIGH)
    pinMode(EPD_CS, OUTPUT);
    digitalWrite(EPD_CS, HIGH);

    // Ensure SD CS is deasserted (HIGH)
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);

    // Initialize SPI2 interface
    SPI2.begin();
    pinMode(SD_MISO, INPUT_PULLUP);

    // Send 16 dummy clock cycles (2 bytes of 0xFF) with SD CS HIGH
    // on SPI2 to force the SD card to release MISO and enter stable state.
    SPI2.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    SPI2.transfer(0xFF);
    SPI2.transfer(0xFF);
    SPI2.endTransaction();

    if (_sdInitialized) {
        // Verify the card is still inserted, initialized, and responsive to commands.
        // We read the Card Identification Register (CID) to perform a fast hardware check.
        cid_t cid;
        if (sd.card() && sd.card()->errorCode() == 0 && sd.card()->readCID(&cid)) {
            return true;
        }

        // If the card was working and now fails, it has crashed/desynced.
        Serial.println("[SD Debug] Card desync detected! Attempting software recovery...");
        if (recoverSDSoftware()) {
            return true;
        }

        // If software recovery fails, power-cycle it.
        Serial.println("[SD Debug] Software recovery failed! Power-cycling peripherals...");
        powerCyclePeripherals();
        _sdInitialized = false;
    }
    
    Serial.println("[SD Debug] Initializing SD Card...");

    // Initialize SdFat using SdSpiConfig at 4 MHz clock speed.
    // By passing USER_SPI_BEGIN, we prevent SdFat from calling SPI.begin() internally.
    if (sd.begin(SdSpiConfig(SD_CS, USER_SPI_BEGIN, SD_SCK_MHZ(4), &SPI2))) {
        _sdInitialized = true;
        Serial.println("[SD Debug] SD Card initialized successfully at 4 MHz.");
        return true;
    }
    
    // Fallback: try lower speed (2 MHz)
    Serial.println("[SD Debug] 4 MHz failed, retrying at 2 MHz...");
    if (sd.begin(SdSpiConfig(SD_CS, USER_SPI_BEGIN, SD_SCK_MHZ(2), &SPI2))) {
        _sdInitialized = true;
        Serial.println("[SD Debug] SD Card initialized at 2 MHz.");
        return true;
    }

    // Fallback: try lowest speed (1 MHz)
    Serial.println("[SD Debug] 2 MHz failed, retrying at 1 MHz...");
    if (sd.begin(SdSpiConfig(SD_CS, USER_SPI_BEGIN, SD_SCK_MHZ(1), &SPI2))) {
        _sdInitialized = true;
        Serial.println("[SD Debug] SD Card initialized at 1 MHz.");
        return true;
    }

    // Attempt software recovery as fallback before physical power cycle
    Serial.println("[SD Debug] Standard init failed. Trying software recovery as fallback...");
    if (recoverSDSoftware()) {
        _sdInitialized = true;
        return true;
    }

    // Both failed. This means the card might be latched in a hardware error state.
    // We will perform a physical power cycle on the VCC line (controlled by Pin 13).
    Serial.println("[SD Debug] SD Card initialization failed! Power-cycling peripherals...");
    powerCyclePeripherals();

    // Try to initialize again after power cycle
    Serial.println("[SD Debug] Retrying SD Card initialization after power cycle...");
    if (sd.begin(SdSpiConfig(SD_CS, USER_SPI_BEGIN, SD_SCK_MHZ(4), &SPI2))) {
        _sdInitialized = true;
        Serial.println("[SD Debug] SD Card recovered successfully at 4 MHz after power cycle.");
        return true;
    }

    if (sd.begin(SdSpiConfig(SD_CS, USER_SPI_BEGIN, SD_SCK_MHZ(2), &SPI2))) {
        _sdInitialized = true;
        Serial.println("[SD Debug] SD Card recovered at 2 MHz after power cycle.");
        return true;
    }

    if (sd.begin(SdSpiConfig(SD_CS, USER_SPI_BEGIN, SD_SCK_MHZ(1), &SPI2))) {
        _sdInitialized = true;
        Serial.println("[SD Debug] SD Card recovered at 1 MHz after power cycle.");
        return true;
    }

    Serial.println("[SD Debug] SD Card hard failure: could not initialize even after power cycle.");
    return false;
}

bool StorageManager::sdExists(const String& path) {
    if (!beginSD()) return false;
    return sd.exists(path.c_str());
}

int StorageManager::listSDBooks(String books[], int maxBooks) {
    if (!beginSD()) return 0;
    
    int count = 0;
    FsFile root = sd.open("/", O_RDONLY);
    if (!root) {
        Serial.println("Failed to open SD root directory!");
        return 0;
    }
    
    FsFile file;
    while (count < maxBooks && file.openNext(&root, O_RDONLY)) {
        char name[100];
        file.getName(name, sizeof(name));
        String filename = String(name);
        
        if (filename.startsWith(".")) {
            file.close();
            continue;
        }
        
        if (file.isDir()) {
            String indexPath = filename + "/index.txt";
            if (sd.exists(indexPath.c_str())) {
                books[count++] = filename + "/";
            }
        } else if (filename.endsWith(".txt")) {
            books[count++] = filename;
        }
        file.close();
    }
    root.close();
    return count;
}

FsFile StorageManager::openSDBook(const String& filename, oflag_t oflag) {
    if (!beginSD()) {
        return FsFile();
    }
    return sd.open(filename.c_str(), oflag);
}

bool StorageManager::enableUSBMSC(bool enable) {
    if (enable) {
        if (!_sdInitialized && !beginSD()) {
            Serial.println("[MSC Debug] Failed to initialize SD card for USB MSC.");
            return false;
        }
        uint32_t sectors = sd.card()->sectorCount();
        usb_msc.setCapacity(sectors, 512);
        usb_msc.setUnitReady(true);
        Serial.println("[MSC Debug] USB MSC enabled. SD card exposed.");
        return true;
    } else {
        usb_msc.setUnitReady(false);
        // Force reinitialization of SD next time it's accessed by MCU
        _sdInitialized = false;
        Serial.println("[MSC Debug] USB MSC disabled. SD card unmounted.");
        return true;
    }
}

bool StorageManager::sdCardReadSectors(uint32_t lba, void* buffer, uint32_t bufsize) {
    if (!_sdInitialized && !beginSD()) return false;
    return sd.card()->readSectors(lba, (uint8_t*)buffer, bufsize / 512);
}

bool StorageManager::sdCardWriteSectors(uint32_t lba, const uint8_t* buffer, uint32_t bufsize) {
    if (!_sdInitialized && !beginSD()) return false;
    return sd.card()->writeSectors(lba, buffer, bufsize / 512);
}

uint32_t StorageManager::sdCardSectorCount() {
    if (!_sdInitialized && !beginSD()) return 0;
    return sd.card()->sectorCount();
}

bool StorageManager::readStats(uint32_t& totalSeconds) {
    if (!_statsLoaded) {
        _statsLoaded = true;
        _cachedTotalSeconds = 0;
        if (beginSD() && sd.exists("/stats.dat")) {
            FsFile file = sd.open("/stats.dat", O_RDONLY);
            if (file) {
                _cachedTotalSeconds = (uint32_t)file.readStringUntil('\n').toInt();
                file.close();
            }
        }
    }
    totalSeconds = _cachedTotalSeconds;
    return _statsLoaded; // true once we've tried to load (even if file didn't exist)
}

bool StorageManager::writeStats(uint32_t totalSeconds) {
    _cachedTotalSeconds = totalSeconds;
    _statsLoaded = true;
    if (InternalFS.exists("/stats.dat")) {
        InternalFS.remove("/stats.dat");
    }
    if (!beginSD()) return false;
    if (sd.exists("/stats.dat")) sd.remove("/stats.dat");
    FsFile file = sd.open("/stats.dat", O_WRONLY | O_CREAT | O_TRUNC);
    if (!file) {
        Serial.println("Failed to open stats.dat for writing.");
        return false;
    }
    file.println(totalSeconds);
    file.close();
    return true;
}

void StorageManager::resetStats() {
    _cachedTotalSeconds = 0;
    _statsLoaded = true; // mark loaded so we don't re-read the (now-deleted) file
    if (InternalFS.exists("/stats.dat")) {
        InternalFS.remove("/stats.dat");
    }
    if (beginSD() && sd.exists("/stats.dat")) {
        sd.remove("/stats.dat");
    }
}

bool StorageManager::saveFramebuffer(const uint8_t* data, size_t len) {
    if (!data || len == 0) return false;
    if (!beginSD()) return false;
    if (sd.exists("/epd_buffer.dat")) sd.remove("/epd_buffer.dat");
    FsFile file = sd.open("/epd_buffer.dat", O_WRONLY | O_CREAT | O_TRUNC);
    if (!file) {
        Serial.println("[FB] Failed to open epd_buffer.dat for writing.");
        return false;
    }
    size_t written = file.write(data, len);
    file.close();
    if (written != len) {
        Serial.print("[FB] Short write to epd_buffer.dat: ");
        Serial.print(written);
        Serial.print("/");
        Serial.println(len);
        sd.remove("/epd_buffer.dat");
        return false;
    }
    return true;
}

bool StorageManager::loadFramebuffer(uint8_t* data, size_t len) {
    if (!data || len == 0) return false;
    if (!beginSD()) return false;
    if (!sd.exists("/epd_buffer.dat")) return false;
    FsFile file = sd.open("/epd_buffer.dat", O_RDONLY);
    if (!file) return false;
    size_t expected = file.size();
    if (expected != len) {
        // Size mismatch (different panel or partial write). Skip.
        file.close();
        return false;
    }
    size_t got = file.read(data, len);
    file.close();
    if (got != len) return false;
    return true;
}

bool StorageManager::readSettings(int& fontType, int& fontSize, bool& isFlipped, int& lineSpacing, int& contrastMode) {
    // Defaults
    fontType = 0;
    fontSize = 1;
    isFlipped = false;
    lineSpacing = 1;
    contrastMode = 0;

    if (!beginSD()) return false;
    if (!sd.exists("/settings.dat")) return false;

    FsFile file = sd.open("/settings.dat", O_RDONLY);
    if (!file) return false;

    fontType = file.readStringUntil('\n').toInt();
    fontSize = file.readStringUntil('\n').toInt();
    isFlipped = (file.readStringUntil('\n').toInt() == 1);
    lineSpacing = file.readStringUntil('\n').toInt();
    contrastMode = file.readStringUntil('\n').toInt();
    file.close();
    return true;
}

bool StorageManager::writeSettings(int fontType, int fontSize, bool isFlipped, int lineSpacing, int contrastMode) {
    if (!beginSD()) return false;
    if (sd.exists("/settings.dat")) sd.remove("/settings.dat");
    FsFile file = sd.open("/settings.dat", O_WRONLY | O_CREAT | O_TRUNC);
    if (!file) return false;
    file.println(fontType);
    file.println(fontSize);
    file.println(isFlipped ? 1 : 0);
    file.println(lineSpacing);
    file.println(contrastMode);
    file.close();
    return true;
}

bool StorageManager::writeSleepState(int state, int menuIdx, int bookIdx, int sdBookIdx, int pcBookIdx,
                                     int chapterIdx, int readerMenuIdx, const String& activeBook, int activeChapterIdx) {
    if (!beginSD()) return false;
    if (sd.exists("/sleep_state.dat")) sd.remove("/sleep_state.dat");
    FsFile file = sd.open("/sleep_state.dat", O_WRONLY | O_CREAT | O_TRUNC);
    if (!file) return false;
    file.println(state);
    file.println(menuIdx);
    file.println(bookIdx);
    file.println(sdBookIdx);
    file.println(pcBookIdx);
    file.println(chapterIdx);
    file.println(readerMenuIdx);
    file.println(activeBook);
    file.println(activeChapterIdx);
    file.close();
    return true;
}

bool StorageManager::readSleepState(int& state, int& menuIdx, int& bookIdx, int& sdBookIdx, int& pcBookIdx,
                                    int& chapterIdx, int& readerMenuIdx, String& activeBook, int& activeChapterIdx) {
    if (!beginSD()) return false;
    if (!sd.exists("/sleep_state.dat")) return false;
    FsFile file = sd.open("/sleep_state.dat", O_RDONLY);
    if (!file) return false;
    state = file.readStringUntil('\n').toInt();
    menuIdx = file.readStringUntil('\n').toInt();
    bookIdx = file.readStringUntil('\n').toInt();
    sdBookIdx = file.readStringUntil('\n').toInt();
    pcBookIdx = file.readStringUntil('\n').toInt();
    chapterIdx = file.readStringUntil('\n').toInt();
    readerMenuIdx = file.readStringUntil('\n').toInt();
    if (file.available()) activeBook = file.readStringUntil('\n');
    activeBook.trim();
    if (file.available()) activeChapterIdx = file.readStringUntil('\n').toInt();
    file.close();
    return true;
}

void StorageManager::clearSleepState() {
    if (beginSD() && sd.exists("/sleep_state.dat")) {
        sd.remove("/sleep_state.dat");
    }
}

void StorageManager::migrateInternalFSFiles() {
    // Migrate settings.dat
    if (InternalFS.exists("/settings.dat")) {
        Serial.println("[Migration] Found settings.dat on InternalFS, migrating to SD...");
        if (beginSD()) {
            if (sd.exists("/settings.dat")) sd.remove("/settings.dat");
            LfsFile src = InternalFS.open("/settings.dat", FILE_O_READ);
            if (src) {
                FsFile dst = sd.open("/settings.dat", O_WRONLY | O_CREAT | O_TRUNC);
                if (dst) {
                    uint8_t buf[64];
                    int r;
                    while ((r = src.read(buf, sizeof(buf))) > 0) {
                        dst.write(buf, r);
                    }
                    dst.close();
                    Serial.println("[Migration] settings.dat migrated successfully.");
                }
                src.close();
                // Delete from InternalFS after verifying the copy
                InternalFS.remove("/settings.dat");
                Serial.println("[Migration] settings.dat removed from InternalFS.");
            }
        }
    }

    // Migrate sleep_state.dat
    if (InternalFS.exists("/sleep_state.dat")) {
        Serial.println("[Migration] Found sleep_state.dat on InternalFS, migrating to SD...");
        if (beginSD()) {
            if (sd.exists("/sleep_state.dat")) sd.remove("/sleep_state.dat");
            LfsFile src = InternalFS.open("/sleep_state.dat", FILE_O_READ);
            if (src) {
                FsFile dst = sd.open("/sleep_state.dat", O_WRONLY | O_CREAT | O_TRUNC);
                if (dst) {
                    uint8_t buf[64];
                    int r;
                    while ((r = src.read(buf, sizeof(buf))) > 0) {
                        dst.write(buf, r);
                    }
                    dst.close();
                    Serial.println("[Migration] sleep_state.dat migrated successfully.");
                }
                src.close();
                InternalFS.remove("/sleep_state.dat");
                Serial.println("[Migration] sleep_state.dat removed from InternalFS.");
            }
        }
    }

    // Migrate stats.dat
    if (InternalFS.exists("/stats.dat")) {
        Serial.println("[Migration] Found stats.dat on InternalFS, migrating to SD...");
        if (beginSD()) {
            if (sd.exists("/stats.dat")) sd.remove("/stats.dat");
            LfsFile src = InternalFS.open("/stats.dat", FILE_O_READ);
            if (src) {
                FsFile dst = sd.open("/stats.dat", O_WRONLY | O_CREAT | O_TRUNC);
                if (dst) {
                    uint8_t buf[64];
                    int r;
                    while ((r = src.read(buf, sizeof(buf))) > 0) {
                        dst.write(buf, r);
                    }
                    dst.close();
                    Serial.println("[Migration] stats.dat migrated successfully.");
                }
                src.close();
                InternalFS.remove("/stats.dat");
                Serial.println("[Migration] stats.dat removed from InternalFS.");
            }
        }
    }

    // Invalidate cached stats so next read picks up the migrated (or new) SD version
    _statsLoaded = false;
}
