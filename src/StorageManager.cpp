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

StorageManager& StorageManager::getInstance() {
    static StorageManager instance;
    return instance;
}

StorageManager::StorageManager() : _uploadFile(nullptr), _isUploading(false), _sdInitialized(false) {}

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
    String fullPath = String(BOOK_DIR) + "/" + filename;
    String bmkPath = fullPath + ".bmk";
    if (InternalFS.exists(bmkPath.c_str())) {
        InternalFS.remove(bmkPath.c_str());
    }
    if (InternalFS.exists(fullPath.c_str())) {
        return InternalFS.remove(fullPath.c_str());
    }
    return false;
}

uint32_t StorageManager::getUsedSpace() {
    uint32_t used = 0;
    LfsFile dir = InternalFS.open(BOOK_DIR, FILE_O_READ);
    if (!dir) return 0;
    
    LfsFile file(InternalFS);
    while ((file = dir.openNextFile())) {
        String name = file.name();
        if (name != "temp.txt" && !name.startsWith(".")) {
            used += file.size();
        }
        file.close();
    }
    dir.close();
    return used;
}

bool StorageManager::writeProgress(const String& currentBook, uint32_t offset) {
    if (InternalFS.exists(PROGRESS_FILE)) {
        InternalFS.remove(PROGRESS_FILE);
    }

    LfsFile progressFile = InternalFS.open(PROGRESS_FILE, FILE_O_WRITE);
    if (!progressFile) {
        Serial.println("Failed to open progress file for writing.");
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
    if (!InternalFS.exists(PROGRESS_FILE)) {
        currentBook = "";
        offset = 0;
        return false;
    }

    LfsFile progressFile = InternalFS.open(PROGRESS_FILE, FILE_O_READ);
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
    if (!InternalFS.exists(PROGRESS_FILE)) {
        currentBook = "";
        offset = 0;
        historyCount = 0;
        return false;
    }

    LfsFile progressFile = InternalFS.open(PROGRESS_FILE, FILE_O_READ);
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
    String path = String(BOOK_DIR) + "/" + getBookmarkFilename(filename) + ".bmk";
    if (InternalFS.exists(path.c_str())) {
        InternalFS.remove(path.c_str());
    }

    LfsFile progressFile = InternalFS.open(path.c_str(), FILE_O_WRITE);
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
    String path = String(BOOK_DIR) + "/" + getBookmarkFilename(filename) + ".bmk";
    if (!InternalFS.exists(path.c_str())) {
        offset = 0;
        return false;
    }
    LfsFile progressFile = InternalFS.open(path.c_str(), FILE_O_READ);
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
    String path = String(BOOK_DIR) + "/" + getBookmarkFilename(filename) + ".bmk";
    if (!InternalFS.exists(path.c_str())) {
        offset = 0;
        historyCount = 0;
        return false;
    }
    LfsFile progressFile = InternalFS.open(path.c_str(), FILE_O_READ);
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
    if (_isUploading && _uploadFile != nullptr) {
        _uploadFile->close();
        delete _uploadFile;
        _uploadFile = nullptr;
    }
    
    // Always write to temp.txt first to ensure atomic updates
    String tempPath = String(BOOK_DIR) + "/temp.txt";
    if (InternalFS.exists(tempPath.c_str())) {
        InternalFS.remove(tempPath.c_str());
    }

    // Dynamically allocate File with InternalFS reference
    _uploadFile = new Adafruit_LittleFS_Namespace::File(InternalFS);
    *_uploadFile = InternalFS.open(tempPath.c_str(), FILE_O_WRITE);
    if (!(*_uploadFile)) {
        Serial.println("Failed to open temp.txt for writing BLE stream.");
        delete _uploadFile;
        _uploadFile = nullptr;
        _isUploading = false;
        return false;
    }

    _isUploading = true;
    Serial.println("Started writing to temp.txt...");
    return true;
}

bool StorageManager::writeBookChunk(const uint8_t* data, size_t len) {
    if (!_isUploading || _uploadFile == nullptr || !(*_uploadFile)) {
        Serial.println("[Device Debug] writeBookChunk: Not uploading or file not open.");
        return false;
    }
    size_t written = _uploadFile->write(data, len);
    if (written != len) {
        Serial.print("[Device Debug] writeBookChunk: Flash write failed! Expected ");
        Serial.print(len);
        Serial.print(" bytes, wrote ");
        Serial.println(written);
        return false;
    }
    return true;
}

bool StorageManager::finalizeBookWrite(const String& title) {
    if (!_isUploading || _uploadFile == nullptr) {
        Serial.println("[Device Debug] finalizeBookWrite: Not uploading or file pointer null.");
        return false;
    }
    _uploadFile->close();
    delete _uploadFile;
    _uploadFile = nullptr;
    _isUploading = false;

    String tempPath = String(BOOK_DIR) + "/temp.txt";
    uint32_t tempSize = 0;

    // Check temp.txt size
    LfsFile tempFile = InternalFS.open(tempPath.c_str(), FILE_O_READ);
    if (tempFile) {
        tempSize = tempFile.size();
        Serial.print("[Device Debug] temp.txt size: ");
        Serial.print(tempSize);
        Serial.println(" bytes.");
        tempFile.close();
    } else {
        Serial.println("[Device Debug] Warning: Failed to open temp.txt to check size.");
    }

    // Open temp.txt, extract the first line as book title if title is empty
    String finalTitle = title;
    finalTitle.trim();

    if (finalTitle.length() == 0) {
        tempFile = InternalFS.open(tempPath.c_str(), FILE_O_READ);
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

    String finalPath = String(BOOK_DIR) + "/" + cleanTitle;

    if (InternalFS.exists(finalPath.c_str())) {
        InternalFS.remove(finalPath.c_str());
    }

    Serial.print("Renaming temp.txt to: ");
    Serial.println(finalPath);

    if (tempSize == 0) {
        Serial.println("[Device Debug] Warning: Attempting to rename an empty 0-byte file!");
    }

    if (!InternalFS.rename(tempPath.c_str(), finalPath.c_str())) {
        Serial.println("Rename failed!");
        return false;
    }

    return true;
}

bool StorageManager::clearStorage() {
    if (_isUploading && _uploadFile != nullptr) {
        _uploadFile->close();
        delete _uploadFile;
        _uploadFile = nullptr;
        _isUploading = false;
    }
    
    Serial.println("[Device Debug] Formatting filesystem...");
    // Format entire flash filesystem to apply the new partition size (320KB) and clear any corruption
    InternalFS.end();
    if (!InternalFS.format()) {
        Serial.println("Format failed!");
        return false;
    }
    if (!InternalFS.begin()) {
        Serial.println("Re-mount failed!");
        return false;
    }
    Serial.println("[Device Debug] Filesystem formatted successfully with 320KB layout.");
    return createBookDir();
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
