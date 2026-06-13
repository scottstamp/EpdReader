#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include <Arduino.h>
#include "Config.h"
#include <Adafruit_GFX.h>

enum FontType { FONT_SANS, FONT_SERIF, FONT_MONO, FONT_LITERATA, FONT_ATKINSON };
enum FontSize { SIZE_SMALL, SIZE_MEDIUM, SIZE_LARGE };

#include <Adafruit_GFX.h>

class UIView {
public:
    virtual ~UIView() {}
    virtual void render(Adafruit_GFX& display) = 0;
    virtual bool prefersFullRefresh() { return false; }
};

class ReaderView : public UIView {
public:
    ReaderView(const String& filename, uint32_t startOffset, uint32_t& nextPageOffset);
    void render(Adafruit_GFX& display) override;
    bool prefersFullRefresh() override;
private:
    String _filename;
    uint32_t _startOffset;
    uint32_t& _nextPageOffset;
};

class MenuView : public UIView {
public:
    MenuView(const String& header, const String options[], int count, int selectedIdx);
    void render(Adafruit_GFX& display) override;
private:
    String _header;
    const String* _options;
    int _count;
    int _selectedIdx;
};

class MessageView : public UIView {
public:
    MessageView(const String& title, const String& msg, bool isAlert = false);
    void render(Adafruit_GFX& display) override;
private:
    String _title;
    String _msg;
    bool _isAlert;
};

class ProgressView : public UIView {
public:
    ProgressView(const String& task, int percentage);
    void render(Adafruit_GFX& display) override;
private:
    String _task;
    int _percentage;
};

class DisplayManager {
public:
    static DisplayManager& getInstance();

    void begin();
    void clear();
    void powerDown();
    void powerUp();

    // Unified render interface
    void draw(UIView& view);

    // Page History Management (for Previous Page functionality)
    void clearHistory();
    void pushHistory(uint32_t offset);
    uint32_t popHistory();
    bool hasHistory() const;
    int getHistoryOffsets(uint32_t* dest, int maxLen);
    void setHistoryOffsets(const uint32_t* src, int count);

    // Font Configuration
    FontType getFontType() const { return _fontType; }
    FontSize getFontSize() const { return _fontSize; }
    void setFontType(FontType type);
    void setFontSize(FontSize size);
    void cycleFontType();
    void cycleFontSize();

    // Orientation Configuration
    bool isFlipped() const { return _isFlipped; }
    void setFlipped(bool flipped);

    void loadSettings();
    void saveSettings();

    bool isWakeupFromSleep() const { return _isWakeupFromSleep; }
    void checkAndTriggerPreFetch(const String& filename);

    // Make ReaderView a friend so it can call drawPageText
    friend class ReaderView;

private:
    DisplayManager();
    DisplayManager(const DisplayManager&) = delete;
    DisplayManager& operator=(const DisplayManager&) = delete;

    // Helper pagination drawers
    uint32_t drawPageText(const String& filename, uint32_t startOffset, bool performRender);

    // Page history circular buffer
    uint32_t _history[HISTORY_SIZE];
    int _historyHead;
    int _historyTail;
    int _historyCount;

    // Font settings
    FontType _fontType;
    FontSize _fontSize;

    // Orientation settings
    bool _isFlipped;

    // Sleep wakeup status
    bool _isWakeupFromSleep;
    uint32_t _nextPageOffset;

    // BLE Book text cache
    #define BLE_CACHE_SIZE 4096
    char _bleCacheBuffer[BLE_CACHE_SIZE];
    uint32_t _bleCacheStartOffset;
    uint32_t _bleCacheLength;
    String _bleCachedFilename;

    void clearBleCache();
};

#endif // DISPLAY_MANAGER_H
