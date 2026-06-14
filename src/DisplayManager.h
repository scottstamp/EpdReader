#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include <Arduino.h>
#include "Config.h"
#include <Adafruit_GFX.h>


enum FontType { FONT_SANS, FONT_SERIF, FONT_MONO, FONT_LITERATA, FONT_ATKINSON };
enum FontSize { SIZE_SMALL, SIZE_MEDIUM, SIZE_LARGE };

class UIView {
public:
    virtual ~UIView() {}
    virtual void prepare() {}
    virtual void render(Adafruit_GFX& display) = 0;
    virtual bool prefersFullRefresh() { return false; }
};

class ReaderView : public UIView {
public:
    ReaderView(const String& filename, uint32_t startOffset, uint32_t& nextPageOffset,
               bool isChapterized = false, const String& chapterTitle = "", uint32_t chapterSize = 0);
    void prepare() override;
    void render(Adafruit_GFX& display) override;
    bool prefersFullRefresh() override;
private:
    String _filename;
    uint32_t _startOffset;
    uint32_t& _nextPageOffset;
    bool _isChapterized;
    String _chapterTitle;
    uint32_t _chapterSize;
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

    bool getDisplayNeedsReinit() const { return _displayNeedsReinit; }
    void setDisplayNeedsReinit(bool needs) { _displayNeedsReinit = needs; }

    void setChapterInfo(const String& title, uint32_t size) {
        _currentChapterTitle = title;
        _currentChapterSize = size;
    }

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
    bool _displayNeedsReinit;

    // Chapter metadata for rendering
    String _currentChapterTitle;
    uint32_t _currentChapterSize;

    // Unified page text cache (4KB)
    #define PAGE_CACHE_SIZE 4096
    char _pageCacheBuffer[PAGE_CACHE_SIZE];
    int _pageCacheLength;
    String _pageCacheFilename;
    uint32_t _pageCacheStartOffset;
    uint32_t _pageCacheTrueStartOffset;

    void cachePageText(const String& filename, uint32_t startOffset);
    void clearPageCache();
};

#endif // DISPLAY_MANAGER_H
