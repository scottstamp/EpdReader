#include "StorageManager.h"
#include "Config.h"

#include "DisplayManager.h"

using namespace Adafruit_LittleFS_Namespace;

StorageManager& StorageManager::getInstance() {
    static StorageManager instance;
    return instance;
}

StorageManager::StorageManager() : _uploadFile(nullptr), _isUploading(false) {}

bool StorageManager::begin() {
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
    File dir = InternalFS.open(BOOK_DIR, FILE_O_READ);
    if (dir && dir.isDirectory()) {
        File file(InternalFS);
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
    File dir = InternalFS.open(BOOK_DIR, FILE_O_READ);
    if (!dir) {
        Serial.println("Failed to open books directory!");
        return 0;
    }
    
    if (!dir.isDirectory()) {
        Serial.println("Books path is not a directory!");
        return 0;
    }

    File file(InternalFS);
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
    File dir = InternalFS.open(BOOK_DIR, FILE_O_READ);
    if (!dir) return 0;
    
    File file(InternalFS);
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

    File progressFile = InternalFS.open(PROGRESS_FILE, FILE_O_WRITE);
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

    File progressFile = InternalFS.open(PROGRESS_FILE, FILE_O_READ);
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

    File progressFile = InternalFS.open(PROGRESS_FILE, FILE_O_READ);
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

bool StorageManager::writeBookmark(const String& filename, uint32_t offset) {
    String path = String(BOOK_DIR) + "/" + filename + ".bmk";
    if (InternalFS.exists(path.c_str())) {
        InternalFS.remove(path.c_str());
    }

    File progressFile = InternalFS.open(path.c_str(), FILE_O_WRITE);
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
    String path = String(BOOK_DIR) + "/" + filename + ".bmk";
    if (!InternalFS.exists(path.c_str())) {
        offset = 0;
        return false;
    }
    File progressFile = InternalFS.open(path.c_str(), FILE_O_READ);
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
    String path = String(BOOK_DIR) + "/" + filename + ".bmk";
    if (!InternalFS.exists(path.c_str())) {
        offset = 0;
        historyCount = 0;
        return false;
    }
    File progressFile = InternalFS.open(path.c_str(), FILE_O_READ);
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

File StorageManager::openBook(const String& filename, const char* mode) {
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
    _uploadFile = new File(InternalFS);
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
    File tempFile = InternalFS.open(tempPath.c_str(), FILE_O_READ);
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
