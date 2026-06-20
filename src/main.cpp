#include <Arduino.h>
#include "Config.h"
#include "StorageManager.h"
#include "ButtonManager.h"
#include "DisplayManager.h"
#include "BleManager.h"

using namespace Adafruit_LittleFS_Namespace;
using LfsFile = Adafruit_LittleFS_Namespace::File;

static_assert(PINS_COUNT == 38, "Custom variant not loaded! PINS_COUNT must be 38.");

enum SystemState
{
    STATE_BOOT,
    STATE_MENU,
    STATE_BOOK_LIST,
    STATE_PC_BROWSE,
    STATE_READER,
    STATE_BLE_UPLOAD,
    STATE_READER_MENU,
    STATE_SD_BROWSE,
    STATE_USB_MSC,
    STATE_CHAPTER_LIST,
    STATE_FONT_SETTINGS,
    STATE_STATS
};

SystemState currentState = STATE_BOOT;
uint32_t lastActivityTime = 0;

volatile bool bleRequestNextPage = false;
volatile bool bleRequestPrevPage = false;

bool isSystemInReaderMode()
{
    return currentState == STATE_READER;
}

// Book Reader States
String activeBookFilename = "";
String activeBookTitle = "";
uint32_t currentPageOffset = 0;
uint32_t nextPageOffset = 0;
uint32_t activeBookSize = 0;

struct ChapterInfo
{
    String title;
    String filename;
    uint32_t size;
};

ChapterInfo bookChapters[MAX_CHAPTERS];
int bookChapterCount = 0;
int activeChapterIdx = 0;
bool isActiveBookChapterized = false;

// Menu variables
String menuOptions[6];
int menuCount = 0;
int selectedMenuIdx = 0;

// Book list variables
String bookList[MAX_BOOKS];
int bookCount = 0;
int selectedBookIdx = 0;

// PC Browse variables
String pcBookList[MAX_BOOKS];
int pcBookCount = 0;
int selectedPcBookIdx = 0;
bool resumeOnConnect = false;
bool pcListFetched = false;

// SD Browse variables
String sdBookList[MAX_BOOKS];
int sdBookCount = 0;
int selectedSdBookIdx = 0;

// BLE Upload Status
String bleStatusMsg = "Waiting for connection...";
int bleBytesReceived = 0;
bool bleUploadFinished = false;

// Reader Menu variables
String readerMenuOptions[8];
int readerMenuCount = 7;
int selectedReaderMenuIdx = 0;
int selectedChapterIdx = 0;
String chapterMenuOptions[MAX_CHAPTERS];
bool restoringSleepState = false;

// Font Settings submenu variables
String fontMenuOptions[6];
int fontMenuCount = 0;
int selectedFontMenuIdx = 0;

// Reading statistics
uint32_t totalReadingSeconds = 0;     // Persisted cumulative seconds
uint32_t currentSessionStartMs = 0;   // millis() at which current session began
uint32_t currentSessionSeconds = 0;   // Accumulated seconds in the active session

// Function declarations
void transitionTo(SystemState newState);
void handleMenu();
void handleBookList();
void handlePcBrowse();
void handleSdBrowse();
void handleReader();
void handleBleUpload();
void handleReaderMenu();
void drawReaderMenu();
void handleChapterList();
void handleSerialUpload();
void enterDeepSleep();
void bleStateCallback(bool connected);
void bleProgressCallback(const String &status, int bytesReceived, bool finished);
void handleUsbMsc();
void handleFontSettings();
void handleStats();
void drawFontSettingsMenu();
void accumulateSessionTime();
uint32_t currentSessionSecondsLive();
String formatDuration(uint32_t totalSeconds);
void beginReadingSession();
void endReadingSession();

String getBookTitleFromPath(const String &path)
{
    String name = path;
    if (name.startsWith("[SD]"))
        name = name.substring(4);
    if (name.endsWith("/"))
        name = name.substring(0, name.length() - 1);
    int lastSlash = name.lastIndexOf('/');
    if (lastSlash >= 0)
        name = name.substring(lastSlash + 1);
    name.replace("_", " ");
    return name;
}

bool loadBookChapters(const String &bookPath)
{
    isActiveBookChapterized = false;
    bookChapterCount = 0;
    activeChapterIdx = 0;

    if (!bookPath.startsWith("[SD]"))
    {
        return false;
    }

    String folderName = bookPath.substring(4); // e.g. "The_Hobbit/"
    String indexPath = folderName + "index.txt";

    Serial.print("[Chapters] Loading index: ");
    Serial.println(indexPath);

    FsFile indexFile = StorageManager::getInstance().openSDBook(indexPath, O_RDONLY);
    if (!indexFile)
    {
        Serial.println("[Chapters] Failed to open index.txt");
        return false;
    }

    while (indexFile.available() && bookChapterCount < MAX_CHAPTERS)
    {
        String line = indexFile.readStringUntil('\n');
        line.trim();
        if (line.length() == 0 || line.startsWith("#"))
        {
            continue; // Skip empty lines and comments
        }

        int colonIdx = line.indexOf(':');
        String title = "";
        String filename = "";

        if (colonIdx >= 0)
        {
            title = line.substring(0, colonIdx);
            filename = line.substring(colonIdx + 1);
        }
        else
        {
            filename = line;
            title = filename;
            if (title.endsWith(".txt"))
            {
                title = title.substring(0, title.length() - 4);
            }
            title.replace("_", " ");
        }
        title.trim();
        filename.trim();

        if (filename.length() > 0)
        {
            String fullPath = folderName + filename;
            FsFile chapFile = StorageManager::getInstance().openSDBook(fullPath, O_RDONLY);
            if (chapFile)
            {
                bookChapters[bookChapterCount].title = title;
                bookChapters[bookChapterCount].filename = fullPath;
                bookChapters[bookChapterCount].size = chapFile.size();
                chapFile.close();

                Serial.print("[Chapters] Loaded Chapter ");
                Serial.print(bookChapterCount);
                Serial.print(": ");
                Serial.print(title);
                Serial.print(" (");
                Serial.print(filename);
                Serial.print(", ");
                Serial.print(bookChapters[bookChapterCount].size);
                Serial.println(" bytes)");

                bookChapterCount++;
            }
            else
            {
                Serial.print("[Chapters] Warning: chapter file not found: ");
                Serial.println(fullPath);
            }
        }
    }
    indexFile.close();

    if (bookChapterCount > 0)
    {
        isActiveBookChapterized = true;
        return true;
    }
    return false;
}

void setup()
{
    uint32_t t0 = millis();
    Serial.begin(115200);
    // Wait for Serial to open if USB plugged in (non-blocking)
    uint32_t startSerial = millis();
    while (!Serial && (millis() - startSerial < 1000))
        ;

    Serial.println("\n--- nRF52840 e-Paper Book Reader ---");

    uint32_t t = millis();
    Serial.print("[T+"); Serial.print(t - t0); Serial.println("ms] Initializing battery...");
    analogReference(AR_INTERNAL_3_0); // 3.0V reference
    analogReadResolution(10);         // 10-bit resolution

    t = millis();
    Serial.print("[T+"); Serial.print(t - t0); Serial.println("ms] Initializing Storage (InternalFS)...");
    if (!StorageManager::getInstance().begin())
    {
        Serial.println("Storage initialization failed!");
    }

    t = millis();
    Serial.print("[T+"); Serial.print(t - t0); Serial.println("ms] Migrating InternalFS -> SD...");
    StorageManager::getInstance().migrateInternalFSFiles();

    t = millis();
    Serial.print("[T+"); Serial.print(t - t0); Serial.println("ms] Reading stats from SD...");
    uint32_t loadedSeconds = 0;
    if (StorageManager::getInstance().readStats(loadedSeconds)) {
        totalReadingSeconds = loadedSeconds;
        Serial.print("Total reading time: ");
        Serial.print(totalReadingSeconds);
        Serial.println(" sec");
    }

    t = millis();
    Serial.print("[T+"); Serial.print(t - t0); Serial.println("ms] Initializing Buttons...");
    ButtonManager::getInstance().begin();

    t = millis();
    Serial.print("[T+"); Serial.print(t - t0); Serial.println("ms] Initializing Display (EPD)...");
    DisplayManager::getInstance().begin();

    // BLE disabled — USB upload is faster and BLE is not currently used.
    // t = millis();
    // Serial.print("[T+"); Serial.print(t - t0); Serial.println("ms] Initializing BLE...");
    // BleManager::getInstance().begin(bleStateCallback, bleProgressCallback);
    // BleManager::getInstance().startAdvertising();

    t = millis();
    Serial.print("[T+"); Serial.print(t - t0); Serial.println("ms] Setup subsystems done.");
    lastActivityTime = millis();
    Serial.print("[T+"); Serial.print(millis() - t0); Serial.println("ms] Setup completed successfully.");
    // Check if we have a saved sleep state (now on SD)
    uint32_t t_before_sleep = millis();
    bool restoredFromSleepState = false;
    int savedState = 0, savedMenuIdx = 0, savedBookIdx = 0, savedSdBookIdx = 0;
    int savedPcBookIdx = 0, savedChapterIdx = 0, savedReaderMenuIdx = 0, savedActiveChapterIdx = 0;
    String savedActiveBook = "";
    if (StorageManager::getInstance().readSleepState(savedState, savedMenuIdx, savedBookIdx,
        savedSdBookIdx, savedPcBookIdx, savedChapterIdx, savedReaderMenuIdx, savedActiveBook, savedActiveChapterIdx))
    {
        // Clear it so it's a one-time restore
        StorageManager::getInstance().clearSleepState();

        if (savedState == STATE_MENU || savedState == STATE_BOOK_LIST ||
            savedState == STATE_SD_BROWSE || savedState == STATE_PC_BROWSE ||
            savedState == STATE_CHAPTER_LIST || savedState == STATE_READER_MENU)
        {
            selectedMenuIdx = savedMenuIdx;
            selectedBookIdx = savedBookIdx;
            selectedSdBookIdx = savedSdBookIdx;
            selectedPcBookIdx = savedPcBookIdx;
            selectedChapterIdx = savedChapterIdx;
            selectedReaderMenuIdx = savedReaderMenuIdx;

            if (savedActiveBook.length() > 0)
            {
                activeBookFilename = savedActiveBook;
                if (savedActiveBook.startsWith("[SD]"))
                {
                    int slashIdx = savedActiveBook.indexOf('/', 4);
                    if (slashIdx >= 0)
                    {
                        String folderPath = savedActiveBook.substring(0, slashIdx + 1);
                        loadBookChapters(folderPath);
                    }
                }
            }
            activeChapterIdx = savedActiveChapterIdx;

            restoringSleepState = true;
            transitionTo((SystemState)savedState);
            restoringSleepState = false;
            restoredFromSleepState = true;
        }
    }

    uint32_t t_after_sleep = millis();
    Serial.print("[T+"); Serial.print(t_after_sleep - t0); Serial.println("ms] Sleep state restore done.");
    (void)t_before_sleep; // unused

    if (!restoredFromSleepState)
    {
        // 5. Automatic restore reading progress on startup
        String savedBook = "";
        uint32_t savedOffset = 0;
        uint32_t hist[HISTORY_SIZE];
        int histCount = 0;
        if (StorageManager::getInstance().readProgress(savedBook, savedOffset, hist, HISTORY_SIZE, histCount) && savedBook.length() > 0)
        {
            Serial.print("Found reading progress: ");
            Serial.print(savedBook);
            Serial.print(" at offset ");
            Serial.println(savedOffset);

            // Open file to check it exists and read metadata
            if (savedBook.startsWith("[SD]"))
            {
                String realFilename = savedBook.substring(4);

                // Check if savedBook is inside a chapterized subdirectory
                int slashIdx = savedBook.indexOf('/', 4);
                if (slashIdx >= 0)
                {
                    String folderPath = savedBook.substring(0, slashIdx + 1); // e.g. "[SD]The_Hobbit/"
                    if (loadBookChapters(folderPath))
                    {
                        int foundIdx = -1;
                        for (int i = 0; i < bookChapterCount; i++)
                        {
                            if (bookChapters[i].filename == realFilename)
                            {
                                foundIdx = i;
                                break;
                            }
                        }
                        if (foundIdx >= 0)
                        {
                            activeChapterIdx = foundIdx;
                            activeBookFilename = savedBook;
                            activeBookSize = bookChapters[activeChapterIdx].size;
                            activeBookTitle = getBookTitleFromPath(folderPath);

                            // Normalize: ensure currentPageOffset always has chapter index in upper byte.
                            // Old saves may have had plain file offsets without chapter encoding.
                            uint32_t fileOffsetOnly = savedOffset & 0x00FFFFFF;
                            currentPageOffset = ((uint32_t)activeChapterIdx << 24) | fileOffsetOnly;

                            DisplayManager::getInstance().clearHistory();
                            if (histCount > 0)
                            {
                                // Re-encode any history entries that lack a chapter index
                                // (entries from old saves will have 0 in the upper byte if on chapter 0,
                                // or garbage if they were plain offsets — normalize them all)
                                uint32_t normHist[HISTORY_SIZE];
                                for (int i = 0; i < histCount; i++)
                                {
                                    // If the saved history entry has 0 in the chapter byte but we're
                                    // on a chapter > 0, it was a plain offset — re-encode it.
                                    // If it already has a chapter index encoded, leave it as-is.
                                    uint32_t hEntry = hist[i];
                                    if ((hEntry >> 24) == 0 && activeChapterIdx > 0)
                                    {
                                        hEntry = ((uint32_t)activeChapterIdx << 24) | (hEntry & 0x00FFFFFF);
                                    }
                                    normHist[i] = hEntry;
                                }
                                DisplayManager::getInstance().setHistoryOffsets(normHist, histCount);
                            }
                            else
                            {
                                DisplayManager::getInstance().pushHistory(currentPageOffset);
                            }

                            transitionTo(STATE_READER);
                            return;
                        }
                    }
                }

                FsFile file = StorageManager::getInstance().openSDBook(realFilename, O_RDONLY);
                if (file)
                {
                    activeBookFilename = savedBook;
                    activeBookSize = file.size();

                    activeBookTitle = file.readStringUntil('\n');
                    activeBookTitle.trim();
                    file.close();

                    currentPageOffset = savedOffset;

                    DisplayManager::getInstance().clearHistory();
                    if (histCount > 0)
                    {
                        DisplayManager::getInstance().setHistoryOffsets(hist, histCount);
                    }
                    else
                    {
                        uint32_t textStart = activeBookTitle.length() + 1;
                        DisplayManager::getInstance().pushHistory(textStart);
                        if (currentPageOffset > textStart)
                        {
                            DisplayManager::getInstance().pushHistory(currentPageOffset);
                        }
                    }

                    transitionTo(STATE_READER);
                    return;
                }
            }
            else
            {
                LfsFile file = StorageManager::getInstance().openBook(savedBook, "r");
                if (file)
                {
                    activeBookFilename = savedBook;
                    activeBookSize = file.size();

                    // Read first line as title
                    activeBookTitle = file.readStringUntil('\n');
                    activeBookTitle.trim();
                    file.close();

                    currentPageOffset = savedOffset;

                    // Push very first page offset to history to initialize back navigation
                    DisplayManager::getInstance().clearHistory();
                    if (histCount > 0)
                    {
                        DisplayManager::getInstance().setHistoryOffsets(hist, histCount);
                    }
                    else
                    {
                        uint32_t textStart = activeBookTitle.length() + 1;
                        DisplayManager::getInstance().pushHistory(textStart);
                        if (currentPageOffset > textStart)
                        {
                            DisplayManager::getInstance().pushHistory(currentPageOffset);
                        }
                    }

                    transitionTo(STATE_READER);
                    return;
                }
            }
        }

        // Default to main menu
        transitionTo(STATE_MENU);
    }
}

void loop()
{
    // BLE disabled — BLE stack is not initialized, so skip update.
    // BleManager::getInstance().update();

    // Poll and debounce button presses
    ButtonManager::getInstance().update();

    // Check USB Serial for incoming book uploads
    if (currentState != STATE_USB_MSC)
    {
        handleSerialUpload();
    }

    switch (currentState)
    {
    case STATE_MENU:
        handleMenu();
        break;
    case STATE_BOOK_LIST:
        handleBookList();
        break;
    case STATE_PC_BROWSE:
        handlePcBrowse();
        break;
    case STATE_READER:
        handleReader();
        break;
    case STATE_BLE_UPLOAD:
        handleBleUpload();
        break;
    case STATE_READER_MENU:
        handleReaderMenu();
        break;
    case STATE_SD_BROWSE:
        handleSdBrowse();
        break;
    case STATE_USB_MSC:
        handleUsbMsc();
        break;
    case STATE_CHAPTER_LIST:
        handleChapterList();
        break;
    case STATE_FONT_SETTINGS:
        handleFontSettings();
        break;
    case STATE_STATS:
        handleStats();
        break;
    default:
        break;
    }

    // Handle auto-sleep timeout
    // Do not sleep while connected to BLE, active upload stream, or if Serial is active (USB connected and monitor open)
    bool isBleConnected = BleManager::getInstance().isConnected() || BleManager::getInstance().isCentralConnected();
    bool isBleBusy = isBleConnected || (bleBytesReceived > 0);
    bool isUsbActive = Serial; // Checks if USB Serial is active/connected
    bool usbBlocksSleep = isUsbActive;
#if !ENABLE_USB_SLEEP_BLOCK
    usbBlocksSleep = false; // override: allow sleep even with USB active
#endif

    if (currentState != STATE_USB_MSC && !isBleBusy && !usbBlocksSleep && (millis() - lastActivityTime > AUTO_SLEEP_MS))
    {
        // Persist any accumulated reading time before sleeping
        if (currentState == STATE_READER) {
            accumulateSessionTime();
            StorageManager::getInstance().writeStats(totalReadingSeconds);
        }
        enterDeepSleep();
    }

    // yield to FreeRTOS
    delay(10);
}

void transitionTo(SystemState newState)
{
    bool isInitialTransition = (currentState == STATE_BOOT);

    // Manage reading session: close any active session before leaving STATE_READER
    if (currentState == STATE_READER && newState != STATE_READER) {
        endReadingSession();
    }

    currentState = newState;
    lastActivityTime = millis();
    if (!isInitialTransition) {
        ButtonManager::getInstance().reset();
    }

    // Open a new reading session when entering the reader
    if (newState == STATE_READER) {
        beginReadingSession();
    }

    // Enable PC stream bypass in BleManager if we are in PC Browse or Reader with a BLE book
    bool isBleBook = activeBookFilename.startsWith("[BLE]");
    bool pcStream = (newState == STATE_PC_BROWSE) || (newState == STATE_READER && isBleBook);
    BleManager::getInstance().setPCStreamActive(pcStream);

    if (newState == STATE_MENU)
    {
        Serial.println("Transition to: STATE_MENU");
        // Rebuild menu items
        menuCount = 0;

        // Show "Resume" option only if there is valid progress
        String savedBook = "";
        uint32_t savedOffset = 0;
        if (StorageManager::getInstance().readProgress(savedBook, savedOffset) && savedBook.length() > 0)
        {
            menuOptions[menuCount++] = "Resume Reading";
        }

        menuOptions[menuCount++] = "Book List"; // Represents SD books
        menuOptions[menuCount++] = "USB SD Reader";
        menuOptions[menuCount++] = "BLE Upload Mode";
        menuOptions[menuCount++] = "Reading Stats";
        menuOptions[menuCount++] = "Clear Storage";

        if (!restoringSleepState) {
            selectedMenuIdx = 0;
        }

        // Render Menu Screen
        MenuView menu("Main Menu", menuOptions, menuCount, selectedMenuIdx);
        DisplayManager::getInstance().draw(menu);
    }
    else if (newState == STATE_BOOK_LIST)
    {
        Serial.println("Transition to: STATE_BOOK_LIST");
        // Retrieve books
        bookCount = StorageManager::getInstance().listBooks(bookList, MAX_BOOKS);
        if (!restoringSleepState) {
            selectedBookIdx = 0;
        }

        if (bookCount == 0)
        {
            // Show error message
            MessageView msg("Book List", "No books found!\nSelect BLE Upload Mode\nfrom the main menu\nto transfer books.", false);
            DisplayManager::getInstance().draw(msg);
        }
        else
        {
            // Render list
            // Clean filenames for display (remove .txt extension)
            String cleanNames[MAX_BOOKS];
            for (int i = 0; i < bookCount; i++)
            {
                cleanNames[i] = bookList[i];
                if (cleanNames[i].endsWith(".txt"))
                {
                    cleanNames[i] = cleanNames[i].substring(0, cleanNames[i].length() - 4);
                }
                // Replace underscores with spaces for premium look
                cleanNames[i].replace("_", " ");
            }
            MenuView menu("Select Book", cleanNames, bookCount, selectedBookIdx);
            DisplayManager::getInstance().draw(menu);
        }
    }
    else if (newState == STATE_PC_BROWSE)
    {
        Serial.println("Transition to: STATE_PC_BROWSE");
        if (!restoringSleepState) {
            selectedPcBookIdx = 0;
        }
        pcListFetched = false;

        bool isConnected = BleManager::getInstance().isCentralConnected() || BleManager::getInstance().isConnected();
        if (isConnected)
        {
            if (resumeOnConnect)
            {
                resumeOnConnect = false;
                String realFilename = activeBookFilename.substring(5);

                MessageView msg("Resuming", "Fetching book metrics...", false);
                DisplayManager::getInstance().draw(msg);

                activeBookSize = BleManager::getInstance().requestBookSize(realFilename);
                activeBookTitle = realFilename;
                if (activeBookTitle.endsWith(".txt"))
                {
                    activeBookTitle = activeBookTitle.substring(0, activeBookTitle.length() - 4);
                }
                activeBookTitle.replace("_", " ");

                DisplayManager::getInstance().clearHistory();
                String savedBook = "";
                uint32_t savedOffset = 0;
                uint32_t hist[HISTORY_SIZE];
                int histCount = 0;
                if (StorageManager::getInstance().readProgress(savedBook, savedOffset, hist, HISTORY_SIZE, histCount) && histCount > 0)
                {
                    DisplayManager::getInstance().setHistoryOffsets(hist, histCount);
                }
                else
                {
                    char tempBuf[128];
                    int len = BleManager::getInstance().requestBookText(realFilename, 0, tempBuf, sizeof(tempBuf) - 1);
                    tempBuf[len] = '\0';
                    char *newlinePtr = strchr(tempBuf, '\n');
                    uint32_t textStart = 0;
                    if (newlinePtr)
                    {
                        textStart = (newlinePtr - tempBuf) + 1;
                    }
                    DisplayManager::getInstance().pushHistory(textStart);
                    if (currentPageOffset > textStart)
                    {
                        DisplayManager::getInstance().pushHistory(currentPageOffset);
                    }
                }
                transitionTo(STATE_READER);
                return;
            }

            MessageView msg("PC Books", "Retrieving book list...", false);
            DisplayManager::getInstance().draw(msg);

            if (BleManager::getInstance().requestBookList(pcBookList, MAX_BOOKS, pcBookCount))
            {
                pcListFetched = true;
                if (pcBookCount == 0)
                {
                    MessageView noBooksMsg("PC Books", "No books found on PC.", false);
                    DisplayManager::getInstance().draw(noBooksMsg);
                    delay(1500);
                    transitionTo(STATE_MENU);
                }
                else
                {
                    String cleanNames[MAX_BOOKS];
                    for (int i = 0; i < pcBookCount; i++)
                    {
                        cleanNames[i] = pcBookList[i];
                        if (cleanNames[i].endsWith(".txt"))
                        {
                            cleanNames[i] = cleanNames[i].substring(0, cleanNames[i].length() - 4);
                        }
                        cleanNames[i].replace("_", " ");
                    }
                    MenuView menu("Select PC Book", cleanNames, pcBookCount, selectedPcBookIdx);
                    DisplayManager::getInstance().draw(menu);
                }
            }
            else
            {
                MessageView errMsg("Error", "Failed to get book list.\nReturning to menu...", true);
                DisplayManager::getInstance().draw(errMsg);
                delay(1500);
                transitionTo(STATE_MENU);
            }
        }
        else
        {
            BleManager::getInstance().startScanning();
            MessageView msg("PC Books", "Scanning for PC...\nMake sure ble_server.py\nis running on your PC.", false);
            DisplayManager::getInstance().draw(msg);
        }
    }
    else if (newState == STATE_READER)
    {
        Serial.println("Transition to: STATE_READER");
        // Render current page
        if (isActiveBookChapterized)
        {
            uint32_t fileOffset = currentPageOffset & 0x00FFFFFF;
            ReaderView reader(activeBookFilename, fileOffset, nextPageOffset,
                              true, bookChapters[activeChapterIdx].title, bookChapters[activeChapterIdx].size);
            DisplayManager::getInstance().draw(reader);
        }
        else
        {
            ReaderView reader(activeBookFilename, currentPageOffset, nextPageOffset,
                              false, "", activeBookSize);
            DisplayManager::getInstance().draw(reader);
        }
    }
    else if (newState == STATE_BLE_UPLOAD)
    {
        Serial.println("Transition to: STATE_BLE_UPLOAD");
        bleStatusMsg = "Waiting for connection...";
        bleBytesReceived = 0;
        bleUploadFinished = false;

        // Turn on VCC to power up display and stabilize BLE
        ButtonManager::getInstance().setPeripheralPower(true);
        BleManager::getInstance().startAdvertising();

        MessageView msg("BLE Upload", "Device Name: nRF_Epd_Reader\n\nStatus: Advertising BLE...\nUse nRF Connect or NUS App\nto stream a book text file.", false);
        DisplayManager::getInstance().draw(msg);
    }
    else if (newState == STATE_READER_MENU)
    {
        Serial.println("Transition to: STATE_READER_MENU");
        if (!restoringSleepState) {
            selectedReaderMenuIdx = 0;
        }
        drawReaderMenu();
    }
    else if (newState == STATE_SD_BROWSE)
    {
        Serial.println("Transition to: STATE_SD_BROWSE");
        if (!restoringSleepState) {
            selectedSdBookIdx = 0;
        }

        MessageView msg("Book List", "Initializing SD Card...", false);
        DisplayManager::getInstance().draw(msg);

        sdBookCount = StorageManager::getInstance().listSDBooks(sdBookList, MAX_BOOKS);

        if (sdBookCount == 0)
        {
            MessageView msg("Book List", "No books found!\nEnsure card is exFAT/FAT\nand contains .txt files\nin the root directory.", false);
            DisplayManager::getInstance().draw(msg);
        }
        else
        {
            String cleanNames[MAX_BOOKS + 1];
            for (int i = 0; i < sdBookCount; i++)
            {
                cleanNames[i] = sdBookList[i];
                if (cleanNames[i].endsWith(".txt"))
                {
                    cleanNames[i] = cleanNames[i].substring(0, cleanNames[i].length() - 4);
                }
                else if (cleanNames[i].endsWith("/"))
                {
                    cleanNames[i] = cleanNames[i].substring(0, cleanNames[i].length() - 1);
                }
                cleanNames[i].replace("_", " ");
            }
            cleanNames[sdBookCount] = "[Back]";
            MenuView menu("Select Book", cleanNames, sdBookCount + 1, selectedSdBookIdx);
            DisplayManager::getInstance().draw(menu);
        }
    }
    else if (newState == STATE_USB_MSC)
    {
        Serial.println("Transition to: STATE_USB_MSC");
        // Ensure peripheral power is ON so SD card has VCC
        ButtonManager::getInstance().setPeripheralPower(true);
        delay(50); // Wait for power to stabilize

        if (StorageManager::getInstance().enableUSBMSC(true))
        {
            MessageView msg("USB SD Reader", "SD card exposed over USB.\n\nPress select to exit.", false);
            DisplayManager::getInstance().draw(msg);
        }
        else
        {
            MessageView msg("USB Error", "Failed to expose SD card.\nReturning to menu...", true);
            DisplayManager::getInstance().draw(msg);
            delay(2000);
            transitionTo(STATE_MENU);
        }
    }
    else if (newState == STATE_CHAPTER_LIST)
    {
        Serial.println("Transition to: STATE_CHAPTER_LIST");
        selectedChapterIdx = activeChapterIdx;

        for (int i = 0; i < bookChapterCount; i++)
        {
            chapterMenuOptions[i] = bookChapters[i].title;
        }
        MenuView menu("Select Chapter", chapterMenuOptions, bookChapterCount, selectedChapterIdx);
        DisplayManager::getInstance().draw(menu);
    }
    else if (newState == STATE_FONT_SETTINGS)
    {
        Serial.println("Transition to: STATE_FONT_SETTINGS");
        if (!restoringSleepState) {
            selectedFontMenuIdx = 0;
        }
        drawFontSettingsMenu();
    }
    else if (newState == STATE_STATS)
    {
        Serial.println("Transition to: STATE_STATS");
        String body = "Total reading time:\n" + formatDuration(totalReadingSeconds) + "\n\nCurrent session:\n" + formatDuration(currentSessionSecondsLive());
        MessageView msg("Reading Stats", body, false);
        DisplayManager::getInstance().draw(msg);
    }
}

void handleMenu()
{
    ButtonEvent prev = ButtonManager::getInstance().getPrevEvent();
    ButtonEvent next = ButtonManager::getInstance().getNextEvent();
    ButtonEvent select = ButtonManager::getInstance().getSelectEvent();

    if (prev == BTN_CLICK)
    {
        lastActivityTime = millis();
        selectedMenuIdx = (selectedMenuIdx - 1 + menuCount) % menuCount;
        MenuView menu("Main Menu", menuOptions, menuCount, selectedMenuIdx);
        DisplayManager::getInstance().draw(menu);
    }
    else if (next == BTN_CLICK)
    {
        lastActivityTime = millis();
        selectedMenuIdx = (selectedMenuIdx + 1) % menuCount;
        MenuView menu("Main Menu", menuOptions, menuCount, selectedMenuIdx);
        DisplayManager::getInstance().draw(menu);
    }
    else if (select == BTN_CLICK)
    {
        lastActivityTime = millis();
        String selection = menuOptions[selectedMenuIdx];

        if (selection == "Resume Reading")
        {
            String savedBook = "";
            uint32_t savedOffset = 0;
            uint32_t hist[HISTORY_SIZE];
            int histCount = 0;
            if (StorageManager::getInstance().readProgress(savedBook, savedOffset, hist, HISTORY_SIZE, histCount))
            {
                if (savedBook.startsWith("[BLE]"))
                {
                    activeBookFilename = savedBook;
                    currentPageOffset = savedOffset;
                    resumeOnConnect = true;
                    transitionTo(STATE_PC_BROWSE);
                }
                else if (savedBook.startsWith("[SD]"))
                {
                    String realFilename = savedBook.substring(4);
                    // Check if this is a chapterized book (path contains a slash, e.g. "The_Compound/p1c6.txt")
                    int slashIdx = realFilename.indexOf('/');
                    if (slashIdx > 0)
                    {
                        // Chapterized book – restore full chapter state
                        String folderPath = "[SD]" + realFilename.substring(0, slashIdx + 1);
                        if (loadBookChapters(folderPath))
                        {
                            int foundIdx = 0;
                            for (int i = 0; i < bookChapterCount; i++)
                            {
                                if (bookChapters[i].filename == realFilename)
                                {
                                    foundIdx = i;
                                    break;
                                }
                            }
                            activeChapterIdx = foundIdx;
                            activeBookFilename = savedBook;
                            activeBookSize = bookChapters[activeChapterIdx].size;
                            activeBookTitle = getBookTitleFromPath(folderPath);

                            // Normalize offset to always carry chapter index in upper byte
                            uint32_t fileOffsetOnly = savedOffset & 0x00FFFFFF;
                            currentPageOffset = ((uint32_t)activeChapterIdx << 24) | fileOffsetOnly;

                            DisplayManager::getInstance().clearHistory();
                            if (histCount > 0)
                            {
                                uint32_t normHist[HISTORY_SIZE];
                                for (int i = 0; i < histCount; i++)
                                {
                                    uint32_t hEntry = hist[i];
                                    if ((hEntry >> 24) == 0 && activeChapterIdx > 0)
                                    {
                                        hEntry = ((uint32_t)activeChapterIdx << 24) | (hEntry & 0x00FFFFFF);
                                    }
                                    normHist[i] = hEntry;
                                }
                                DisplayManager::getInstance().setHistoryOffsets(normHist, histCount);
                            }
                            else
                            {
                                DisplayManager::getInstance().pushHistory(currentPageOffset);
                            }

                            transitionTo(STATE_READER);
                        }
                        else
                        {
                            MessageView errMsg("Error", "Could not load book index.", true);
                            DisplayManager::getInstance().draw(errMsg);
                            delay(1500);
                            transitionTo(STATE_MENU);
                        }
                    }
                    else
                    {
                        // Flat single-file SD book
                        FsFile file = StorageManager::getInstance().openSDBook(realFilename, O_RDONLY);
                        if (file)
                        {
                            activeBookFilename = savedBook;
                            activeBookSize = file.size();
                            activeBookTitle = file.readStringUntil('\n');
                            activeBookTitle.trim();
                            file.close();

                            currentPageOffset = savedOffset;

                            DisplayManager::getInstance().clearHistory();
                            if (histCount > 0)
                            {
                                DisplayManager::getInstance().setHistoryOffsets(hist, histCount);
                            }
                            else
                            {
                                uint32_t textStart = activeBookTitle.length() + 1;
                                DisplayManager::getInstance().pushHistory(textStart);
                                if (currentPageOffset > textStart)
                                {
                                    DisplayManager::getInstance().pushHistory(currentPageOffset);
                                }
                            }

                            transitionTo(STATE_READER);
                        }
                        else
                        {
                            MessageView errMsg("Error", "Could not open SD book.", true);
                            DisplayManager::getInstance().draw(errMsg);
                            delay(1500);
                            transitionTo(STATE_MENU);
                        }
                    }
                }
                else
                {
                    LfsFile file = StorageManager::getInstance().openBook(savedBook, "r");
                    if (file)
                    {
                        activeBookFilename = savedBook;
                        activeBookSize = file.size();
                        activeBookTitle = file.readStringUntil('\n');
                        activeBookTitle.trim();
                        file.close();

                        currentPageOffset = savedOffset;

                        DisplayManager::getInstance().clearHistory();
                        if (histCount > 0)
                        {
                            DisplayManager::getInstance().setHistoryOffsets(hist, histCount);
                        }
                        else
                        {
                            uint32_t textStart = activeBookTitle.length() + 1;
                            DisplayManager::getInstance().pushHistory(textStart);
                            if (currentPageOffset > textStart)
                            {
                                DisplayManager::getInstance().pushHistory(currentPageOffset);
                            }
                        }

                        transitionTo(STATE_READER);
                    }
                }
            }
        }
        else if (selection == "Book List")
        {
            transitionTo(STATE_SD_BROWSE);
        }
        else if (selection == "USB SD Reader")
        {
            transitionTo(STATE_USB_MSC);
        }
        else if (selection == "BLE Upload Mode")
        {
            transitionTo(STATE_BLE_UPLOAD);
        }
        else if (selection == "Reading Stats")
        {
            transitionTo(STATE_STATS);
        }
        else if (selection == "Clear Storage")
        {
            MessageView formattingMsg("Formatting", "Clearing storage files\nPlease wait...", false);
            DisplayManager::getInstance().draw(formattingMsg);
            StorageManager::getInstance().clearStorage();
            DisplayManager::getInstance().saveSettings(); // Restore font preferences to new filesystem
            MessageView clearedMsg("Storage Cleared", "All books and reading\nprogress have been deleted.", false);
            DisplayManager::getInstance().draw(clearedMsg);
            delay(1500);
            transitionTo(STATE_MENU);
        }
    }
    else if (select == BTN_LONG_PRESS)
    {
        lastActivityTime = millis();
        if (activeBookFilename.length() > 0)
        {
            transitionTo(STATE_READER);
        }
    }
}

void handleUsbMsc()
{
    ButtonEvent select = ButtonManager::getInstance().getSelectEvent();
    if (select == BTN_CLICK)
    {
        lastActivityTime = millis();
        StorageManager::getInstance().enableUSBMSC(false);
        ButtonManager::getInstance().setPeripheralPower(false);
        transitionTo(STATE_MENU);
    }
}

void handleBookList()
{
    ButtonEvent prev = ButtonManager::getInstance().getPrevEvent();
    ButtonEvent next = ButtonManager::getInstance().getNextEvent();
    ButtonEvent select = ButtonManager::getInstance().getSelectEvent();

    // If no books are found, any select action or long press goes back
    if (bookCount == 0)
    {
        if (select == BTN_CLICK || select == BTN_LONG_PRESS || prev == BTN_CLICK || next == BTN_CLICK)
        {
            transitionTo(STATE_MENU);
        }
        return;
    }

    if (prev == BTN_CLICK)
    {
        lastActivityTime = millis();
        selectedBookIdx = (selectedBookIdx - 1 + bookCount) % bookCount;

        String cleanNames[MAX_BOOKS];
        for (int i = 0; i < bookCount; i++)
        {
            cleanNames[i] = bookList[i];
            if (cleanNames[i].endsWith(".txt"))
                cleanNames[i] = cleanNames[i].substring(0, cleanNames[i].length() - 4);
            cleanNames[i].replace("_", " ");
        }
        MenuView menu("Select Book", cleanNames, bookCount, selectedBookIdx);
        DisplayManager::getInstance().draw(menu);
    }
    else if (next == BTN_CLICK)
    {
        lastActivityTime = millis();
        selectedBookIdx = (selectedBookIdx + 1) % bookCount;

        String cleanNames[MAX_BOOKS];
        for (int i = 0; i < bookCount; i++)
        {
            cleanNames[i] = bookList[i];
            if (cleanNames[i].endsWith(".txt"))
                cleanNames[i] = cleanNames[i].substring(0, cleanNames[i].length() - 4);
            cleanNames[i].replace("_", " ");
        }
        MenuView menu("Select Book", cleanNames, bookCount, selectedBookIdx);
        DisplayManager::getInstance().draw(menu);
    }
    else if (select == BTN_CLICK)
    {
        lastActivityTime = millis();

        // Open book
        activeBookFilename = bookList[selectedBookIdx];
        LfsFile file = StorageManager::getInstance().openBook(activeBookFilename, "r");
        if (file)
        {
            activeBookSize = file.size();
            activeBookTitle = file.readStringUntil('\n');
            activeBookTitle.trim();
            file.close();

            // Start at beginning (which will dynamically resolve past the metadata first line)
            currentPageOffset = 0;

            DisplayManager::getInstance().clearHistory();
            uint32_t textStart = activeBookTitle.length() + 1;
            DisplayManager::getInstance().pushHistory(textStart);

            // Write progress
            StorageManager::getInstance().writeProgress(activeBookFilename, textStart);

            transitionTo(STATE_READER);
        }
        else
        {
            MessageView errMsg("Error", "Could not open selected book.", true);
            DisplayManager::getInstance().draw(errMsg);
            delay(1500);
            transitionTo(STATE_BOOK_LIST);
        }
    }
    else if (select == BTN_LONG_PRESS)
    {
        transitionTo(STATE_MENU);
    }
}

void handleReader()
{
    bool isConnected = BleManager::getInstance().isCentralConnected() || BleManager::getInstance().isConnected();
    if (activeBookFilename.startsWith("[BLE]") && !isConnected)
    {
        MessageView msg("Connection Lost", "Lost connection to PC.\nReturning to menu...", true);
        DisplayManager::getInstance().draw(msg);
        delay(2000);
        transitionTo(STATE_MENU);
        return;
    }

    if (isConnected && activeBookFilename.startsWith("[BLE]"))
    {
        DisplayManager::getInstance().checkAndTriggerPreFetch(activeBookFilename);
    }

    ButtonEvent prev = ButtonManager::getInstance().getPrevEvent();
    ButtonEvent next = ButtonManager::getInstance().getNextEvent();
    ButtonEvent select = ButtonManager::getInstance().getSelectEvent();

    if (next == BTN_CLICK || bleRequestNextPage)
    {
        bleRequestNextPage = false;
        lastActivityTime = millis();

        if (isActiveBookChapterized)
        {
            uint32_t nextOffsetOnly = nextPageOffset & 0x00FFFFFF;
            if (nextOffsetOnly < activeBookSize && nextPageOffset != (currentPageOffset & 0x00FFFFFF))
            {
                // Re-encode chapter index into the stored offset so history always carries chapter info
                currentPageOffset = ((uint32_t)activeChapterIdx << 24) | (nextOffsetOnly & 0x00FFFFFF);
                DisplayManager::getInstance().pushHistory(currentPageOffset);
                StorageManager::getInstance().writeProgress(activeBookFilename, currentPageOffset);
                transitionTo(STATE_READER);
            }
            else
            {
                if (activeChapterIdx + 1 < bookChapterCount)
                {
                    activeChapterIdx++;
                    activeBookFilename = "[SD]" + bookChapters[activeChapterIdx].filename;
                    activeBookSize = bookChapters[activeChapterIdx].size;

                    currentPageOffset = ((uint32_t)activeChapterIdx << 24) | 0;
                    DisplayManager::getInstance().pushHistory(currentPageOffset);
                    StorageManager::getInstance().writeProgress(activeBookFilename, currentPageOffset);
                    transitionTo(STATE_READER);
                }
                else
                {
                    MessageView msg("The End", "You have finished reading:\n" + activeBookTitle + "\n\nPress select to return.", false);
                    DisplayManager::getInstance().draw(msg);
                    currentPageOffset = ((uint32_t)activeChapterIdx << 24) | (nextPageOffset & 0x00FFFFFF);
                }
            }
        }
        else
        {
            if (nextPageOffset < activeBookSize && nextPageOffset != currentPageOffset)
            {
                currentPageOffset = nextPageOffset;
                DisplayManager::getInstance().pushHistory(currentPageOffset);
                StorageManager::getInstance().writeProgress(activeBookFilename, currentPageOffset);
                transitionTo(STATE_READER);
            }
            else
            {
                MessageView msg("The End", "You have finished reading:\n" + activeBookTitle + "\n\nPress select to return.", false);
                DisplayManager::getInstance().draw(msg);
                currentPageOffset = nextPageOffset;
            }
        }
    }
    else if (prev == BTN_CLICK || bleRequestPrevPage)
    {
        bleRequestPrevPage = false;
        lastActivityTime = millis();
        if (DisplayManager::getInstance().hasHistory())
        {
            uint32_t prevEncodedOffset = DisplayManager::getInstance().popHistory();

            if (isActiveBookChapterized)
            {
                int prevChapterIdx = prevEncodedOffset >> 24;
                if (prevChapterIdx != activeChapterIdx)
                {
                    activeChapterIdx = prevChapterIdx;
                    activeBookFilename = "[SD]" + bookChapters[activeChapterIdx].filename;
                    activeBookSize = bookChapters[activeChapterIdx].size;
                }
            }

            currentPageOffset = prevEncodedOffset;
            StorageManager::getInstance().writeProgress(activeBookFilename, currentPageOffset);
            transitionTo(STATE_READER);
        }
    }
    else if (select == BTN_CLICK)
    {
        lastActivityTime = millis();
        transitionTo(STATE_READER_MENU);
    }
    else if (select == BTN_LONG_PRESS)
    {
        lastActivityTime = millis();
        transitionTo(STATE_MENU);
    }
}

void handleBleUpload()
{
    ButtonEvent select = ButtonManager::getInstance().getSelectEvent();

    // Press select or long press to stop advertising and go back to menu
    if (select == BTN_CLICK || select == BTN_LONG_PRESS)
    {
        BleManager::getInstance().stopAdvertising();
        transitionTo(STATE_MENU);
        return;
    }

    if (bleUploadFinished)
    {
        // If finished, wait for any button action to return to menu
        ButtonEvent prev = ButtonManager::getInstance().getPrevEvent();
        ButtonEvent next = ButtonManager::getInstance().getNextEvent();
        if (prev != BTN_NONE || next != BTN_NONE || select != BTN_NONE)
        {
            bleUploadFinished = false;
            bleBytesReceived = 0;
            transitionTo(STATE_MENU);
        }
    }
}

void bleStateCallback(bool connected)
{
    lastActivityTime = millis();
    if (connected)
    {
        bleStatusMsg = "Connected!";
        if (currentState == STATE_BLE_UPLOAD)
        {
            MessageView msg("BLE Upload", "Status: Connected!\n\nStreaming text file...", false);
            DisplayManager::getInstance().draw(msg);
        }
    }
    else
    {
        bleStatusMsg = "Disconnected.";
        if (currentState == STATE_BLE_UPLOAD)
        {
            MessageView msg("BLE Upload", "Status: Disconnected.\n\nWaiting for connection...", false);
            DisplayManager::getInstance().draw(msg);
        }
    }
}

void bleProgressCallback(const String &status, int bytesReceived, bool finished)
{
    lastActivityTime = millis();
    bleBytesReceived = bytesReceived;

    if (finished)
    {
        bleUploadFinished = true;
        BleManager::getInstance().stopAdvertising();

        if (currentState == STATE_BLE_UPLOAD)
        {
            // Calculate size in KB
            float sizeKB = bytesReceived / 1024.0;
            String sizeMsg = String(sizeKB, 1) + " KB";
            MessageView msg("Upload Complete", status + "\nSize: " + sizeMsg + "\n\nPress any button to return.", false);
            DisplayManager::getInstance().draw(msg);
        }
        else
        {
            // Reset upload flags silently if completed in background
            bleUploadFinished = false;
            bleBytesReceived = 0;
        }
    }
    else
    {
        // Do nothing! Do not refresh the slow e-Paper screen during data stream.
    }
}

// ==================== Reading Session Helpers ====================

void beginReadingSession()
{
    currentSessionStartMs = millis();
    currentSessionSeconds = 0;
}

void endReadingSession()
{
    accumulateSessionTime();
    // Persist cumulative total
    StorageManager::getInstance().writeStats(totalReadingSeconds);
    currentSessionStartMs = 0;
}

void accumulateSessionTime()
{
    if (currentSessionStartMs == 0) return;
    uint32_t now = millis();
    uint32_t elapsedMs = now - currentSessionStartMs;
    currentSessionStartMs = now;
    uint32_t elapsedSec = elapsedMs / 1000;
    currentSessionSeconds += elapsedSec;
    totalReadingSeconds += elapsedSec;
}

uint32_t currentSessionSecondsLive()
{
    if (currentSessionStartMs == 0) return currentSessionSeconds;
    uint32_t now = millis();
    uint32_t elapsedMs = now - currentSessionStartMs;
    return currentSessionSeconds + (elapsedMs / 1000);
}

String formatDuration(uint32_t totalSeconds)
{
    uint32_t hours = totalSeconds / 3600;
    uint32_t minutes = (totalSeconds % 3600) / 60;
    uint32_t seconds = totalSeconds % 60;
    if (hours > 0) {
        return String(hours) + "h " + String(minutes) + "m";
    }
    if (minutes > 0) {
        return String(minutes) + "m " + String(seconds) + "s";
    }
    return String(seconds) + "s";
}

void enterDeepSleep()
{
    Serial.println("System idle timeout! Entering Deep Sleep (System OFF)...");

    // Save state to SD before going to sleep
    StorageManager::getInstance().writeSleepState(
        currentState, selectedMenuIdx, selectedBookIdx, selectedSdBookIdx,
        selectedPcBookIdx, selectedChapterIdx, selectedReaderMenuIdx,
        activeBookFilename, activeChapterIdx);

    // 1. Save ePaper framebuffer to SD so we can restore it on wake.
    //    The IT8951 controller's RAM is wiped when VCC turns off (step 3).
    //    Saving the buffer here lets us LDIM it back to the controller on
    //    wake (no refresh), allowing the next view render to be a fast partial
    //    refresh instead of a slow full refresh.
    DisplayManager::getInstance().saveFramebufferToSD();

    // 2. Power down e-Paper screen (puts controller in deep sleep)
    DisplayManager::getInstance().powerDown();

    // 3. Terminate VCC power pin 13 to cut off all peripheral leaks
    ButtonManager::getInstance().setPeripheralPower(false);

    // 4. Stop Bluetooth stack advertising/connection (no-op if BLE disabled)
    BleManager::getInstance().stopAdvertising();

    // 4. Re-configure the button GPIOs to hardware-sense LOW levels to trigger wakeup.
    //    pinMode(INPUT_PULLUP_SENSE) sets DIR=input, INPUT=connect, PULL=pullup,
    //    and SENSE=LOW. We follow up with an explicit register write to guarantee
    //    the SENSE field is SENSE_LOW (0b11) on every button pin — some pins on
    //    the nRF52840 don't reliably retain sense configuration across the
    //    VCC-off / SPI-busy sequence that precedes this point.
    pinMode(PIN_BTN_PREV, INPUT_PULLUP_SENSE);
    pinMode(PIN_BTN_NEXT, INPUT_PULLUP_SENSE);
    pinMode(PIN_BTN_SELECT, INPUT_PULLUP_SENSE);
    extern const uint32_t g_ADigitalPinMap[];
    auto setSenseLow = [](uint8_t arduinoPin) {
        uint32_t nrfPin = g_ADigitalPinMap[arduinoPin];
        uint32_t cnf = NRF_GPIO->PIN_CNF[nrfPin];
        cnf &= ~GPIO_PIN_CNF_SENSE_Msk;          // clear SENSE bits
        cnf |= (3UL << GPIO_PIN_CNF_SENSE_Pos);   // set SENSE_LOW (0b11)
        NRF_GPIO->PIN_CNF[nrfPin] = cnf;
    };
    setSenseLow(PIN_BTN_PREV);
    setSenseLow(PIN_BTN_NEXT);
    setSenseLow(PIN_BTN_SELECT);

    // 5. Set magic retention flag to identify deep sleep wakeup on boot
    sd_power_gpregret_clr(0, 0xFF);
    sd_power_gpregret_set(0, 0x55);

    // 6. Enter System OFF. We use the direct register write
    //    NRF_POWER->SYSTEMOFF = 1 instead of sd_power_system_off() because the
    //    SoftDevice may not be enabled (BLE is disabled, no SoftDevice bringup).
    //    The direct register write works regardless of SoftDevice state and
    //    produces identical System OFF behavior.
    NRF_POWER->SYSTEMOFF = 1;

    // Should be unreachable — chip is now in System OFF.
    // If we somehow get here, halt the CPU so we don't keep printing.
    __disable_irq();
    while (1) { __WFE(); }
}

void drawReaderMenu()
{
    int percent = 0;
    uint32_t fileOffset = currentPageOffset;
    if (isActiveBookChapterized)
    {
        fileOffset = currentPageOffset & 0x00FFFFFF;
    }
    if (activeBookSize > 0)
    {
        percent = (fileOffset * 100) / activeBookSize;
    }

    readerMenuOptions[0] = "Resume Reading";
    readerMenuOptions[1] = "Bookmark Page";

    uint32_t bmkOffset = 0;
    bool hasBmk = StorageManager::getInstance().readBookmark(activeBookFilename, bmkOffset);
    if (hasBmk)
    {
        int bmkPercent = 0;
        uint32_t bmkFileOffset = bmkOffset & 0x00FFFFFF;
        uint32_t bmkChapterIdx = bmkOffset >> 24;

        uint32_t bmkChapSize = activeBookSize;
        if (isActiveBookChapterized && bmkChapterIdx < (uint32_t)bookChapterCount)
        {
            bmkChapSize = bookChapters[bmkChapterIdx].size;
        }

        if (bmkChapSize > 0)
        {
            bmkPercent = (bmkFileOffset * 100) / bmkChapSize;
        }

        if (isActiveBookChapterized)
        {
            readerMenuOptions[2] = "Go to Bookmark (Ch " + String(bmkChapterIdx + 1) + ", " + String(bmkPercent) + "%)";
        }
        else
        {
            readerMenuOptions[2] = "Go to Bookmark (" + String(bmkPercent) + "%)";
        }
    }
    else
    {
        readerMenuOptions[2] = "Go to Bookmark (None)";
    }

    int idx = 3;
    if (isActiveBookChapterized)
    {
        readerMenuOptions[idx++] = "Select Chapter";
    }

    readerMenuOptions[idx++] = "Font Settings";

    readerMenuOptions[idx++] = "Orientation: " + String(DisplayManager::getInstance().isFlipped() ? "Flipped" : "Normal");
    readerMenuOptions[idx++] = "Exit to Main Menu";

    readerMenuCount = idx;

    String header;
    if (isActiveBookChapterized)
    {
        header = bookChapters[activeChapterIdx].title + " (" + String(percent) + "%)";
    }
    else
    {
        header = "Reader Menu (" + String(percent) + "%)";
    }

    String sessionStr = "Session: " + formatDuration(currentSessionSecondsLive());
    MenuView menu(header, readerMenuOptions, readerMenuCount, selectedReaderMenuIdx, sessionStr);
    DisplayManager::getInstance().draw(menu);
}

void handleReaderMenu()
{
    ButtonEvent prev = ButtonManager::getInstance().getPrevEvent();
    ButtonEvent next = ButtonManager::getInstance().getNextEvent();
    ButtonEvent select = ButtonManager::getInstance().getSelectEvent();

    if (prev == BTN_CLICK)
    {
        lastActivityTime = millis();
        selectedReaderMenuIdx = (selectedReaderMenuIdx - 1 + readerMenuCount) % readerMenuCount;
        drawReaderMenu();
    }
    else if (next == BTN_CLICK)
    {
        lastActivityTime = millis();
        selectedReaderMenuIdx = (selectedReaderMenuIdx + 1) % readerMenuCount;
        drawReaderMenu();
    }
    else if (select == BTN_CLICK)
    {
        lastActivityTime = millis();

        int idx = 0;
        if (selectedReaderMenuIdx == idx++)
        {
            transitionTo(STATE_READER);
        }
        else if (selectedReaderMenuIdx == idx++)
        {
            StorageManager::getInstance().writeBookmark(activeBookFilename, currentPageOffset);
            MessageView msg("Bookmark Set", "Current page bookmark\nsaved successfully.", false);
            DisplayManager::getInstance().draw(msg);
            delay(1200);
            drawReaderMenu();
        }
        else if (selectedReaderMenuIdx == idx++)
        {
            uint32_t bmkOffset = 0;
            uint32_t hist[HISTORY_SIZE];
            int histCount = 0;
            if (StorageManager::getInstance().readBookmark(activeBookFilename, bmkOffset, hist, HISTORY_SIZE, histCount))
            {
                currentPageOffset = bmkOffset;

                if (isActiveBookChapterized)
                {
                    int bmkChapterIdx = bmkOffset >> 24;
                    if (bmkChapterIdx >= bookChapterCount)
                        bmkChapterIdx = 0;
                    activeChapterIdx = bmkChapterIdx;
                    activeBookFilename = "[SD]" + bookChapters[activeChapterIdx].filename;
                    activeBookSize = bookChapters[activeChapterIdx].size;
                }

                StorageManager::getInstance().writeProgress(activeBookFilename, currentPageOffset);

                DisplayManager::getInstance().clearHistory();
                if (histCount > 0)
                {
                    DisplayManager::getInstance().setHistoryOffsets(hist, histCount);
                }
                else
                {
                    DisplayManager::getInstance().pushHistory(currentPageOffset);
                }

                MessageView msg("Jumping...", "Loading bookmark offset\nPlease wait...", false);
                DisplayManager::getInstance().draw(msg);
                delay(1000);
                transitionTo(STATE_READER);
            }
            else
            {
                MessageView msg("No Bookmark", "No bookmark has been\nset for this book.", true);
                DisplayManager::getInstance().draw(msg);
                delay(1200);
                drawReaderMenu();
            }
        }
        else if (isActiveBookChapterized && selectedReaderMenuIdx == idx++)
        {
            transitionTo(STATE_CHAPTER_LIST);
        }
        else if (selectedReaderMenuIdx == idx++)
        {
            // Font Settings submenu
            transitionTo(STATE_FONT_SETTINGS);
        }
        else if (selectedReaderMenuIdx == idx++)
        {
            bool currentFlipped = DisplayManager::getInstance().isFlipped();
            DisplayManager::getInstance().setFlipped(!currentFlipped);
            drawReaderMenu();
        }
        else if (selectedReaderMenuIdx == idx++)
        {
            transitionTo(STATE_MENU);
        }
    }
    else if (select == BTN_LONG_PRESS)
    {
        lastActivityTime = millis();
        transitionTo(STATE_READER);
    }
}

void handleSerialUpload()
{
    if (Serial.available())
    {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();

        if (cmd.startsWith("USB_UPLOAD:"))
        {
            int colonIdx = cmd.indexOf(':', 11);
            uint32_t size = 0;
            String title = "";
            if (colonIdx > 0)
            {
                size = cmd.substring(11, colonIdx).toInt();
                title = cmd.substring(colonIdx + 1);
            }
            else
            {
                size = cmd.substring(11).toInt();
            }
            title.trim();

            Serial.println("USB_READY");
            Serial.flush();

            // Stop BLE advertising during USB upload to prevent SoftDevice radio scheduling and interrupt conflicts
            BleManager::getInstance().stopAdvertising();

            StorageManager::getInstance().startNewBookWrite();
            uint32_t bytesReceived = 0;
            uint32_t lastRead = millis();

            // Statically allocate a 512-byte sector buffer to prevent stack footprint
            static uint8_t sectorBuf[512];
            uint16_t sectorCount = 0;

            while (bytesReceived < size && (millis() - lastRead < 5000))
            {
                if (Serial.available())
                {
                    uint8_t buf[64];
                    uint32_t toRead = min((size - bytesReceived), (uint32_t)sizeof(buf));
                    int count = Serial.readBytes(buf, toRead);
                    if (count > 0)
                    {
                        // Accumulate incoming bytes into sector buffer
                        for (int i = 0; i < count; i++)
                        {
                            sectorBuf[sectorCount++] = buf[i];

                            // Commit sector block to LittleFS once buffer is full
                            if (sectorCount >= sizeof(sectorBuf))
                            {
                                bool writeOk = StorageManager::getInstance().writeBookChunk(sectorBuf, sectorCount);
                                if (!writeOk)
                                {
                                    Serial.print("[Device Debug] USB Serial upload: writeBookChunk failed at ");
                                    Serial.print(bytesReceived - count + i + 1);
                                    Serial.println(" bytes.");
                                }
                                sectorCount = 0;
                                delay(5); // Allow TinyUSB and FreeRTOS tasks to execute
                            }
                        }
                        bytesReceived += count;
                        lastRead = millis();
                    }
                }
                else
                {
                    delay(3); // Yield CPU while waiting for serial bytes
                }
                yield();
            }

            // Flush any remaining bytes in the buffer to flash
            if (sectorCount > 0)
            {
                bool writeOk = StorageManager::getInstance().writeBookChunk(sectorBuf, sectorCount);
                if (!writeOk)
                {
                    Serial.println("[Device Debug] USB Serial upload: Final buffer flush failed.");
                }
            }

            if (bytesReceived >= size)
            {
                bool success = StorageManager::getInstance().finalizeBookWrite(title);
                if (success)
                {
                    Serial.println("USB_SUCCESS");
                }
                else
                {
                    Serial.println("USB_FAILED");
                }
            }
            else
            {
                Serial.println("USB_TIMEOUT");
            }
            Serial.flush();

            // Restart BLE advertising after USB upload completes
            BleManager::getInstance().startAdvertising();
        }
    }
}

void handlePcBrowse()
{
    bool isConnected = BleManager::getInstance().isCentralConnected() || BleManager::getInstance().isConnected();
    if (!isConnected)
    {
        static uint32_t scanStartTime = 0;
        if (scanStartTime == 0)
        {
            scanStartTime = millis();
        }

        // Start scanning as central if not connected (failsafe)
        BleManager::getInstance().startScanning();

        bool nowConnected = BleManager::getInstance().isCentralConnected() || BleManager::getInstance().isConnected();
        if (nowConnected)
        {
            BleManager::getInstance().stopScanning();
            scanStartTime = 0;
            if (resumeOnConnect)
            {
                resumeOnConnect = false;
                String realFilename = activeBookFilename.substring(5);

                MessageView msg("Resuming", "Fetching book metrics...", false);
                DisplayManager::getInstance().draw(msg);

                activeBookSize = BleManager::getInstance().requestBookSize(realFilename);
                activeBookTitle = realFilename;
                if (activeBookTitle.endsWith(".txt"))
                {
                    activeBookTitle = activeBookTitle.substring(0, activeBookTitle.length() - 4);
                }
                activeBookTitle.replace("_", " ");

                DisplayManager::getInstance().clearHistory();
                String savedBook = "";
                uint32_t savedOffset = 0;
                uint32_t hist[HISTORY_SIZE];
                int histCount = 0;
                if (StorageManager::getInstance().readProgress(savedBook, savedOffset, hist, HISTORY_SIZE, histCount) && histCount > 0)
                {
                    DisplayManager::getInstance().setHistoryOffsets(hist, histCount);
                }
                else
                {
                    char tempBuf[128];
                    int len = BleManager::getInstance().requestBookText(realFilename, 0, tempBuf, sizeof(tempBuf) - 1);
                    tempBuf[len] = '\0';
                    char *newlinePtr = strchr(tempBuf, '\n');
                    uint32_t textStart = 0;
                    if (newlinePtr)
                    {
                        textStart = (newlinePtr - tempBuf) + 1;
                    }
                    DisplayManager::getInstance().pushHistory(textStart);
                    if (currentPageOffset > textStart)
                    {
                        DisplayManager::getInstance().pushHistory(currentPageOffset);
                    }
                }
                transitionTo(STATE_READER);
            }
            else
            {
                transitionTo(STATE_PC_BROWSE);
            }
            return;
        }

        if (millis() - scanStartTime > 15000)
        { // 15-second scan timeout
            BleManager::getInstance().stopScanning();
            scanStartTime = 0;
            MessageView msg("Not Found", "EpdBookServer not found.\nReturning to menu...", true);
            DisplayManager::getInstance().draw(msg);
            delay(2000);
            transitionTo(STATE_MENU);
        }
        return;
    }

    if (!pcListFetched)
    {
        BleManager::getInstance().stopScanning();
        if (resumeOnConnect)
        {
            resumeOnConnect = false;
            String realFilename = activeBookFilename.substring(5);

            MessageView msg("Resuming", "Fetching book metrics...", false);
            DisplayManager::getInstance().draw(msg);

            activeBookSize = BleManager::getInstance().requestBookSize(realFilename);
            activeBookTitle = realFilename;
            if (activeBookTitle.endsWith(".txt"))
            {
                activeBookTitle = activeBookTitle.substring(0, activeBookTitle.length() - 4);
            }
            activeBookTitle.replace("_", " ");

            DisplayManager::getInstance().clearHistory();
            String savedBook = "";
            uint32_t savedOffset = 0;
            uint32_t hist[HISTORY_SIZE];
            int histCount = 0;
            if (StorageManager::getInstance().readProgress(savedBook, savedOffset, hist, HISTORY_SIZE, histCount) && histCount > 0)
            {
                DisplayManager::getInstance().setHistoryOffsets(hist, histCount);
            }
            else
            {
                char tempBuf[128];
                int len = BleManager::getInstance().requestBookText(realFilename, 0, tempBuf, sizeof(tempBuf) - 1);
                tempBuf[len] = '\0';
                char *newlinePtr = strchr(tempBuf, '\n');
                uint32_t textStart = 0;
                if (newlinePtr)
                {
                    textStart = (newlinePtr - tempBuf) + 1;
                }
                DisplayManager::getInstance().pushHistory(textStart);
                if (currentPageOffset > textStart)
                {
                    DisplayManager::getInstance().pushHistory(currentPageOffset);
                }
            }
            transitionTo(STATE_READER);
        }
        else
        {
            transitionTo(STATE_PC_BROWSE);
        }
        return;
    }

    ButtonEvent prev = ButtonManager::getInstance().getPrevEvent();
    ButtonEvent next = ButtonManager::getInstance().getNextEvent();
    ButtonEvent select = ButtonManager::getInstance().getSelectEvent();

    if (pcBookCount == 0)
    {
        if (select == BTN_CLICK || select == BTN_LONG_PRESS || prev == BTN_CLICK || next == BTN_CLICK)
        {
            transitionTo(STATE_MENU);
        }
        return;
    }

    if (prev == BTN_CLICK)
    {
        lastActivityTime = millis();
        selectedPcBookIdx = (selectedPcBookIdx - 1 + pcBookCount) % pcBookCount;
        String cleanNames[MAX_BOOKS];
        for (int i = 0; i < pcBookCount; i++)
        {
            cleanNames[i] = pcBookList[i];
            if (cleanNames[i].endsWith(".txt"))
                cleanNames[i] = cleanNames[i].substring(0, cleanNames[i].length() - 4);
            cleanNames[i].replace("_", " ");
        }
        MenuView menu("Select PC Book", cleanNames, pcBookCount, selectedPcBookIdx);
        DisplayManager::getInstance().draw(menu);
    }
    else if (next == BTN_CLICK)
    {
        lastActivityTime = millis();
        selectedPcBookIdx = (selectedPcBookIdx + 1) % pcBookCount;
        String cleanNames[MAX_BOOKS];
        for (int i = 0; i < pcBookCount; i++)
        {
            cleanNames[i] = pcBookList[i];
            if (cleanNames[i].endsWith(".txt"))
                cleanNames[i] = cleanNames[i].substring(0, cleanNames[i].length() - 4);
            cleanNames[i].replace("_", " ");
        }
        MenuView menu("Select PC Book", cleanNames, pcBookCount, selectedPcBookIdx);
        DisplayManager::getInstance().draw(menu);
    }
    else if (select == BTN_CLICK)
    {
        lastActivityTime = millis();
        String selectedBook = pcBookList[selectedPcBookIdx];

        MessageView msg("PC Books", "Connecting to book...", false);
        DisplayManager::getInstance().draw(msg);

        activeBookFilename = "[BLE]" + selectedBook;
        activeBookSize = BleManager::getInstance().requestBookSize(selectedBook);
        activeBookTitle = selectedBook;
        if (activeBookTitle.endsWith(".txt"))
        {
            activeBookTitle = activeBookTitle.substring(0, activeBookTitle.length() - 4);
        }
        activeBookTitle.replace("_", " ");

        currentPageOffset = 0;
        DisplayManager::getInstance().clearHistory();

        char tempBuf[128];
        int len = BleManager::getInstance().requestBookText(selectedBook, 0, tempBuf, sizeof(tempBuf) - 1);
        tempBuf[len] = '\0';
        char *newlinePtr = strchr(tempBuf, '\n');
        uint32_t textStart = 0;
        if (newlinePtr)
        {
            textStart = (newlinePtr - tempBuf) + 1;
        }
        DisplayManager::getInstance().pushHistory(textStart);
        StorageManager::getInstance().writeProgress(activeBookFilename, textStart);

        transitionTo(STATE_READER);
    }
    else if (select == BTN_LONG_PRESS)
    {
        transitionTo(STATE_MENU);
    }
}

void handleSdBrowse()
{
    ButtonEvent prev = ButtonManager::getInstance().getPrevEvent();
    ButtonEvent next = ButtonManager::getInstance().getNextEvent();
    ButtonEvent select = ButtonManager::getInstance().getSelectEvent();

    if (sdBookCount == 0)
    {
        if (select == BTN_CLICK || select == BTN_LONG_PRESS || prev == BTN_CLICK || next == BTN_CLICK)
        {
            transitionTo(STATE_MENU);
        }
        return;
    }

    if (prev == BTN_CLICK)
    {
        lastActivityTime = millis();
        selectedSdBookIdx = (selectedSdBookIdx - 1 + sdBookCount + 1) % (sdBookCount + 1);

        String cleanNames[MAX_BOOKS + 1];
        for (int i = 0; i < sdBookCount; i++)
        {
            cleanNames[i] = sdBookList[i];
            if (cleanNames[i].endsWith(".txt"))
                cleanNames[i] = cleanNames[i].substring(0, cleanNames[i].length() - 4);
            else if (cleanNames[i].endsWith("/"))
                cleanNames[i] = cleanNames[i].substring(0, cleanNames[i].length() - 1);
            cleanNames[i].replace("_", " ");
        }
        cleanNames[sdBookCount] = "[Back]";
        MenuView menu("Select Book", cleanNames, sdBookCount + 1, selectedSdBookIdx);
        DisplayManager::getInstance().draw(menu);
    }
    else if (next == BTN_CLICK)
    {
        lastActivityTime = millis();
        selectedSdBookIdx = (selectedSdBookIdx + 1) % (sdBookCount + 1);

        String cleanNames[MAX_BOOKS + 1];
        for (int i = 0; i < sdBookCount; i++)
        {
            cleanNames[i] = sdBookList[i];
            if (cleanNames[i].endsWith(".txt"))
                cleanNames[i] = cleanNames[i].substring(0, cleanNames[i].length() - 4);
            else if (cleanNames[i].endsWith("/"))
                cleanNames[i] = cleanNames[i].substring(0, cleanNames[i].length() - 1);
            cleanNames[i].replace("_", " ");
        }
        cleanNames[sdBookCount] = "[Back]";
        MenuView menu("Select Book", cleanNames, sdBookCount + 1, selectedSdBookIdx);
        DisplayManager::getInstance().draw(menu);
    }
    else if (select == BTN_CLICK)
    {
        lastActivityTime = millis();

        if (selectedSdBookIdx == sdBookCount)
        {
            transitionTo(STATE_MENU);
            return;
        }

        // Open book
        String filename = sdBookList[selectedSdBookIdx];
        String fullBookPath = "[SD]" + filename;

        // Check if there is saved progress for this specific book in progress.dat
        String savedBook = "";
        uint32_t savedOffset = 0;
        uint32_t hist[HISTORY_SIZE];
        int histCount = 0;
        bool hasProgress = false;

        if (StorageManager::getInstance().readProgress(savedBook, savedOffset, hist, HISTORY_SIZE, histCount))
        {
            if (savedBook == fullBookPath || (fullBookPath.endsWith("/") && savedBook.startsWith(fullBookPath)))
            {
                hasProgress = true;
            }
        }

        // If not the last read book in progress.dat, check if a bookmark exists
        uint32_t bmkOffset = 0;
        bool hasBmk = false;
        if (!hasProgress)
        {
            hasBmk = StorageManager::getInstance().readBookmark(fullBookPath, bmkOffset, hist, HISTORY_SIZE, histCount);
        }

        if (filename.endsWith("/"))
        {
            // Chapterized directory book
            if (loadBookChapters(fullBookPath))
            {
                activeBookTitle = getBookTitleFromPath(fullBookPath);

                if (hasProgress)
                {
                    String realSavedFilename = savedBook.substring(4);
                    int foundIdx = 0;
                    for (int i = 0; i < bookChapterCount; i++)
                    {
                        if (bookChapters[i].filename == realSavedFilename)
                        {
                            foundIdx = i;
                            break;
                        }
                    }
                    activeChapterIdx = foundIdx;
                    activeBookFilename = savedBook;
                    activeBookSize = bookChapters[activeChapterIdx].size;

                    // Normalize offset to always carry chapter index in upper byte
                    uint32_t fileOffsetOnly = savedOffset & 0x00FFFFFF;
                    currentPageOffset = ((uint32_t)activeChapterIdx << 24) | fileOffsetOnly;

                    DisplayManager::getInstance().clearHistory();
                    if (histCount > 0)
                    {
                        uint32_t normHist[HISTORY_SIZE];
                        for (int i = 0; i < histCount; i++)
                        {
                            uint32_t hEntry = hist[i];
                            if ((hEntry >> 24) == 0 && activeChapterIdx > 0)
                            {
                                hEntry = ((uint32_t)activeChapterIdx << 24) | (hEntry & 0x00FFFFFF);
                            }
                            normHist[i] = hEntry;
                        }
                        DisplayManager::getInstance().setHistoryOffsets(normHist, histCount);
                    }
                    else
                    {
                        DisplayManager::getInstance().pushHistory(currentPageOffset);
                    }
                }
                else if (hasBmk)
                {
                    int bmkChapterIdx = bmkOffset >> 24;
                    if (bmkChapterIdx >= bookChapterCount)
                        bmkChapterIdx = 0;
                    activeChapterIdx = bmkChapterIdx;
                    activeBookFilename = "[SD]" + bookChapters[activeChapterIdx].filename;
                    activeBookSize = bookChapters[activeChapterIdx].size;
                    currentPageOffset = bmkOffset;

                    DisplayManager::getInstance().clearHistory();
                    if (histCount > 0)
                    {
                        DisplayManager::getInstance().setHistoryOffsets(hist, histCount);
                    }
                    else
                    {
                        DisplayManager::getInstance().pushHistory(currentPageOffset);
                    }
                    StorageManager::getInstance().writeProgress(activeBookFilename, currentPageOffset);
                }
                else
                {
                    activeChapterIdx = 0;
                    activeBookFilename = "[SD]" + bookChapters[0].filename;
                    activeBookSize = bookChapters[0].size;
                    currentPageOffset = (0 << 24) | 0;

                    DisplayManager::getInstance().clearHistory();
                    DisplayManager::getInstance().pushHistory(currentPageOffset);
                    StorageManager::getInstance().writeProgress(activeBookFilename, currentPageOffset);
                }

                transitionTo(STATE_READER);
            }
            else
            {
                MessageView errMsg("Error", "Could not load book index.txt", true);
                DisplayManager::getInstance().draw(errMsg);
                delay(1500);
                transitionTo(STATE_SD_BROWSE);
            }
        }
        else
        {
            // Single-file book
            isActiveBookChapterized = false;
            bookChapterCount = 0;
            activeChapterIdx = 0;

            FsFile file = StorageManager::getInstance().openSDBook(filename, O_RDONLY);
            if (file)
            {
                activeBookFilename = fullBookPath;
                activeBookSize = file.size();
                activeBookTitle = file.readStringUntil('\n');
                activeBookTitle.trim();
                file.close();

                if (hasProgress)
                {
                    currentPageOffset = savedOffset;
                    DisplayManager::getInstance().clearHistory();
                    if (histCount > 0)
                    {
                        DisplayManager::getInstance().setHistoryOffsets(hist, histCount);
                    }
                    else
                    {
                        uint32_t textStart = activeBookTitle.length() + 1;
                        DisplayManager::getInstance().pushHistory(textStart);
                        if (currentPageOffset > textStart)
                        {
                            DisplayManager::getInstance().pushHistory(currentPageOffset);
                        }
                    }
                }
                else if (hasBmk)
                {
                    currentPageOffset = bmkOffset;
                    DisplayManager::getInstance().clearHistory();
                    if (histCount > 0)
                    {
                        DisplayManager::getInstance().setHistoryOffsets(hist, histCount);
                    }
                    else
                    {
                        uint32_t textStart = activeBookTitle.length() + 1;
                        DisplayManager::getInstance().pushHistory(textStart);
                        if (currentPageOffset > textStart)
                        {
                            DisplayManager::getInstance().pushHistory(currentPageOffset);
                        }
                    }
                    StorageManager::getInstance().writeProgress(activeBookFilename, currentPageOffset);
                }
                else
                {
                    currentPageOffset = 0;
                    DisplayManager::getInstance().clearHistory();
                    uint32_t textStart = activeBookTitle.length() + 1;
                    DisplayManager::getInstance().pushHistory(textStart);
                    StorageManager::getInstance().writeProgress(activeBookFilename, textStart);
                }

                transitionTo(STATE_READER);
            }
            else
            {
                MessageView errMsg("Error", "Could not open SD book.", true);
                DisplayManager::getInstance().draw(errMsg);
                delay(1500);
                transitionTo(STATE_SD_BROWSE);
            }
        }
    }
    else if (select == BTN_LONG_PRESS)
    {
        transitionTo(STATE_MENU);
    }
}

void handleChapterList()
{
    ButtonEvent prev = ButtonManager::getInstance().getPrevEvent();
    ButtonEvent next = ButtonManager::getInstance().getNextEvent();
    ButtonEvent select = ButtonManager::getInstance().getSelectEvent();

    if (prev == BTN_CLICK)
    {
        lastActivityTime = millis();
        selectedChapterIdx = (selectedChapterIdx - 1 + bookChapterCount) % bookChapterCount;

        for (int i = 0; i < bookChapterCount; i++)
        {
            chapterMenuOptions[i] = bookChapters[i].title;
        }
        MenuView menu("Select Chapter", chapterMenuOptions, bookChapterCount, selectedChapterIdx);
        DisplayManager::getInstance().draw(menu);
    }
    else if (next == BTN_CLICK)
    {
        lastActivityTime = millis();
        selectedChapterIdx = (selectedChapterIdx + 1) % bookChapterCount;

        for (int i = 0; i < bookChapterCount; i++)
        {
            chapterMenuOptions[i] = bookChapters[i].title;
        }
        MenuView menu("Select Chapter", chapterMenuOptions, bookChapterCount, selectedChapterIdx);
        DisplayManager::getInstance().draw(menu);
    }
    else if (select == BTN_CLICK)
    {
        lastActivityTime = millis();

        // Jump to selected chapter
        activeChapterIdx = selectedChapterIdx;
        activeBookFilename = "[SD]" + bookChapters[activeChapterIdx].filename;
        activeBookSize = bookChapters[activeChapterIdx].size;

        currentPageOffset = (activeChapterIdx << 24) | 0;

        DisplayManager::getInstance().clearHistory();
        DisplayManager::getInstance().pushHistory(currentPageOffset);
        StorageManager::getInstance().writeProgress(activeBookFilename, currentPageOffset);

        transitionTo(STATE_READER);
    }
    else if (select == BTN_LONG_PRESS)
    {
        lastActivityTime = millis();
        transitionTo(STATE_READER_MENU);
    }
}

// ==================== Font Settings Submenu ====================

void drawFontSettingsMenu()
{
    fontMenuCount = 0;

    FontType fontType = DisplayManager::getInstance().getFontType();
    String fontName = "Sans";
    if (fontType == FONT_SERIF) fontName = "Bookerly";
    else if (fontType == FONT_MONO) fontName = "Mono";
    else if (fontType == FONT_LITERATA) fontName = "Literata";
    else if (fontType == FONT_ATKINSON) fontName = "Atkinson";
    fontMenuOptions[fontMenuCount++] = "Font: " + fontName;

    FontSize fontSize = DisplayManager::getInstance().getFontSize();
    String sizeName = "Small";
    if (fontSize == SIZE_MEDIUM) sizeName = "Medium";
    else if (fontSize == SIZE_LARGE) sizeName = "Large";
    fontMenuOptions[fontMenuCount++] = "Size: " + sizeName;

    LineSpacing lineSpacing = DisplayManager::getInstance().getLineSpacing();
    String spacingName = "Compact";
    if (lineSpacing == SPACING_NORMAL) spacingName = "Normal";
    else if (lineSpacing == SPACING_RELAXED) spacingName = "Relaxed";
    fontMenuOptions[fontMenuCount++] = "Spacing: " + spacingName;

    ContrastMode contrast = DisplayManager::getInstance().getContrastMode();
    String contrastName = (contrast == CONTRAST_INVERTED) ? "Inverted" : "Normal";
    fontMenuOptions[fontMenuCount++] = "Contrast: " + contrastName;

    fontMenuOptions[fontMenuCount++] = "[Back]";

    MenuView menu("Font Settings", fontMenuOptions, fontMenuCount, selectedFontMenuIdx);
    DisplayManager::getInstance().draw(menu);
}

void handleFontSettings()
{
    ButtonEvent prev = ButtonManager::getInstance().getPrevEvent();
    ButtonEvent next = ButtonManager::getInstance().getNextEvent();
    ButtonEvent select = ButtonManager::getInstance().getSelectEvent();

    if (prev == BTN_CLICK)
    {
        lastActivityTime = millis();
        selectedFontMenuIdx = (selectedFontMenuIdx - 1 + fontMenuCount) % fontMenuCount;
        drawFontSettingsMenu();
    }
    else if (next == BTN_CLICK)
    {
        lastActivityTime = millis();
        selectedFontMenuIdx = (selectedFontMenuIdx + 1) % fontMenuCount;
        drawFontSettingsMenu();
    }
    else if (select == BTN_CLICK)
    {
        lastActivityTime = millis();

        if (selectedFontMenuIdx == 0)
        {
            DisplayManager::getInstance().cycleFontType();
            drawFontSettingsMenu();
        }
        else if (selectedFontMenuIdx == 1)
        {
            DisplayManager::getInstance().cycleFontSize();
            drawFontSettingsMenu();
        }
        else if (selectedFontMenuIdx == 2)
        {
            DisplayManager::getInstance().cycleLineSpacing();
            drawFontSettingsMenu();
        }
        else if (selectedFontMenuIdx == 3)
        {
            DisplayManager::getInstance().cycleContrastMode();
            drawFontSettingsMenu();
        }
        else if (selectedFontMenuIdx == 4)
        {
            // Back — return to reader menu
            transitionTo(STATE_READER_MENU);
        }
    }
    else if (select == BTN_LONG_PRESS)
    {
        lastActivityTime = millis();
        transitionTo(STATE_READER_MENU);
    }
}

// ==================== Reading Statistics ====================

void handleStats()
{
    ButtonEvent select = ButtonManager::getInstance().getSelectEvent();
    if (select == BTN_CLICK || select == BTN_LONG_PRESS)
    {
        lastActivityTime = millis();
        transitionTo(STATE_MENU);
    }
}
