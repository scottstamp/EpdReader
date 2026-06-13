#ifndef STORAGE_MANAGER_H
#define STORAGE_MANAGER_H

#include <Arduino.h>
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>

class StorageManager {
public:
    static StorageManager& getInstance();

    bool begin();
    bool createBookDir();
    int listBooks(String books[], int maxBooks);
    bool deleteBook(const String& filename);
    bool writeProgress(const String& currentBook, uint32_t offset);
    bool readProgress(String& currentBook, uint32_t& offset);
    bool readProgress(String& currentBook, uint32_t& offset, uint32_t* historyDest, int maxHistoryLen, int& historyCount);
    bool writeBookmark(const String& filename, uint32_t offset);
    bool readBookmark(const String& filename, uint32_t& offset);
    bool readBookmark(const String& filename, uint32_t& offset, uint32_t* historyDest, int maxHistoryLen, int& historyCount);
    uint32_t getUsedSpace();
    
    // File I/O helpers
    Adafruit_LittleFS_Namespace::File openBook(const String& filename, const char* mode = "r");
    bool startNewBookWrite();
    bool writeBookChunk(const uint8_t* data, size_t len);
    bool finalizeBookWrite(const String& title);
    bool clearStorage();

private:
    StorageManager();
    StorageManager(const StorageManager&) = delete;
    StorageManager& operator=(const StorageManager&) = delete;

    Adafruit_LittleFS_Namespace::File* _uploadFile;
    bool _isUploading;
};

#endif // STORAGE_MANAGER_H
