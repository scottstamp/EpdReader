#include "BleManager.h"
#include "StorageManager.h"
#include "Config.h"
#include "ButtonManager.h"

extern bool isSystemInReaderMode();
extern volatile bool bleRequestNextPage;
extern volatile bool bleRequestPrevPage;

// Instantiate BLE Uart service
BLEUart bleuart;

BleManager* BleManager::_instance = nullptr;

BleManager& BleManager::getInstance() {
    if (!_instance) {
        _instance = new BleManager();
    }
    return *_instance;
}

BleManager::BleManager() : 
    _stateCallback(nullptr), 
    _progressCallback(nullptr), 
    _connected(false), 
    _isTransferring(false), 
    _lastDataTime(0), 
    _bytesReceived(0),
    _connHandle(BLE_CONN_HANDLE_INVALID),
    _expectedUploadSize(0),
    _uploadTitle(""),
    _cmdBuffer(""),
    _centralConnected(false),
    _isScanning(false),
    _centralConnHandle(BLE_CONN_HANDLE_INVALID),
    _pcStreamActive(false),
    _initialized(false)
{
    _instance = this;
}

void BleManager::begin(BleStateCallback stateCb, BleProgressCallback progressCb) {
    _stateCallback = stateCb;
    _progressCallback = progressCb;

    // Turn off automatic connection status LED blinking BEFORE initializing Bluefruit to save power
    Bluefruit.autoConnLed(false);

    // Initialize Bluefruit with 1 Peripheral and 1 Central connection
    bool initOk = Bluefruit.begin(1, 1);
    if (initOk) {
        Serial.println("[BLE Debug] Bluefruit.begin(1, 1) succeeded.");
    } else {
        Serial.println("[BLE Error] Bluefruit.begin(1, 1) failed!");
    }

    Bluefruit.setTxPower(4); // +4 dBm (High power, good range)
    Bluefruit.setName("nRF_Epd_Reader");

    // Force programmable blue status LED pin (P1.10 / digital pin 4) to remain off
    pinMode(4, OUTPUT);
    digitalWrite(4, LOW);

    // Peripheral Callbacks
    Bluefruit.Periph.setConnectCallback(connect_callback);
    Bluefruit.Periph.setDisconnectCallback(disconnect_callback);

    // Central Callbacks
    Bluefruit.Central.setConnectCallback(central_connect_callback);
    Bluefruit.Central.setDisconnectCallback(central_disconnect_callback);

    // Initialize Scanner callbacks
    Bluefruit.Scanner.setRxCallback(scan_callback);
    Bluefruit.Scanner.restartOnDisconnect(true);
    Bluefruit.Scanner.setInterval(160, 80); // interval = 100ms, window = 50ms
    Bluefruit.Scanner.useActiveScan(true);

    // Initialize NUS peripheral service
    bleuart.begin();
    bleuart.setRxCallback(rx_callback);

    // Initialize NUS client service
    _clientUart.begin();

    // Start advertising config
    Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
    Bluefruit.Advertising.addTxPower();
    Bluefruit.Advertising.addService(bleuart); // Advertise NUS uuid
    
    // Put device name in Scan Response to fit under 31-byte limit
    Bluefruit.ScanResponse.addName();
    
    Bluefruit.Advertising.restartOnDisconnect(true);
    Bluefruit.Advertising.setInterval(32, 244); // Fast advertising (20ms to 150.25ms)
    _initialized = true;
}

void BleManager::ensureReady() {
    // No-op.
    // On cold boot: BLE is initialized and advertising starts in setup().
    // On sleep wake: BLE is intentionally left off until the user interacts.
    // This method exists as a placeholder for future lazy-init if needed.
}

void BleManager::startAdvertising() {
    if (!Bluefruit.Advertising.isRunning()) {
        bool startOk = Bluefruit.Advertising.start(0); // 0 = advertise forever
        if (startOk) {
            Serial.println("BLE advertising started successfully.");
        } else {
            Serial.println("[BLE Error] Failed to start BLE advertising!");
        }
    }
}

void BleManager::stopAdvertising() {
    if (Bluefruit.Advertising.isRunning() && !_connected) {
        Bluefruit.Advertising.stop();
        Serial.println("BLE advertising stopped to save battery.");
    }
}

bool BleManager::isConnected() const {
    return _connected;
}

void BleManager::setPCStreamActive(bool active) {
    _pcStreamActive = active;
}

bool BleManager::isPCStreamActive() const {
    return _pcStreamActive;
}

void BleManager::resetTransferState() {
    _isTransferring = false;
    _bytesReceived = 0;
    _expectedUploadSize = 0;
    _uploadTitle = "";
    _cmdBuffer = "";
}

void BleManager::update() {
    // If transferring and no data has arrived for a timeout:
    // For legacy upload (expectedSize == 0), use a 3-second quiet-period finalization.
    // For command-initiated upload, use a longer 10-second timeout as a safety fallback.
    uint32_t timeoutMs = (_expectedUploadSize == 0) ? 3000 : 10000;
    if (_isTransferring && (millis() - _lastDataTime > timeoutMs)) {
        Serial.println("BLE transfer quiet period detected. Saving book...");
        
        // Read first line of temp.txt to get the title if not manually specified
        String title = _uploadTitle;
        if (title.length() == 0) {
            Adafruit_LittleFS_Namespace::File tempFile = StorageManager::getInstance().openBook("temp.txt", "r");
            if (tempFile) {
                title = tempFile.readStringUntil('\n');
                title.trim();
                tempFile.close();
            }
        }

        bool success = StorageManager::getInstance().finalizeBookWrite(title);
        _isTransferring = false;

        if (_expectedUploadSize > 0) {
            bleuart.println("UPLOAD_TIMEOUT");
            bleuart.flush();
            delay(300);
        }

        if (_progressCallback) {
            String status = success ? ("Book: " + (title.length() > 0 ? title : "Uploaded Book")) : "Error saving book";
            _progressCallback(status, _bytesReceived, true);
        }
    }
}

// Static callbacks
void BleManager::connect_callback(uint16_t conn_handle) {
    BLEConnection* conn = Bluefruit.Connection(conn_handle);
    char device_name[32] = { 0 };
    conn->getPeerName(device_name, sizeof(device_name));

    Serial.print("BLE Connected to: ");
    Serial.println(device_name);

    BleManager& instance = BleManager::getInstance();
    instance._connected = true;
    instance._connHandle = conn_handle;
    instance.resetTransferState();

    if (instance._stateCallback) {
        instance._stateCallback(true);
    }
}

void BleManager::disconnect_callback(uint16_t conn_handle, uint8_t reason) {
    (void) conn_handle;
    (void) reason;

    Serial.println("BLE Disconnected.");

    BleManager& instance = BleManager::getInstance();
    instance._connected = false;
    instance._connHandle = BLE_CONN_HANDLE_INVALID;
    
    // If disconnected midway through a transfer, finalize what was received
    if (instance._isTransferring) {
        Serial.println("Disconnected mid-transfer. Saving partial book...");
        bool success = StorageManager::getInstance().finalizeBookWrite("");
        instance._isTransferring = false;
        if (instance._progressCallback) {
            instance._progressCallback(success ? "Partial book saved!" : "Error saving partial book!", instance._bytesReceived, true);
        }
    }

    if (instance._stateCallback) {
        instance._stateCallback(false);
    }
}

void BleManager::rx_callback(uint16_t conn_handle) {
    (void) conn_handle;
    BleManager& instance = BleManager::getInstance();

    // If streaming books from PC as a Peripheral, let the synchronous requests read from bleuart directly
    if (instance._pcStreamActive) {
        return;
    }

    // Read all available bytes from BLE NUS
    while (bleuart.available()) {
        if (instance._isTransferring) {
            // We are in Active Upload Mode (either legacy or command-initiated)
            uint8_t buffer[128];
            int count = bleuart.read(buffer, sizeof(buffer));
            if (count > 0) {
                StorageManager::getInstance().writeBookChunk(buffer, count);
                instance._bytesReceived += count;
                instance._lastDataTime = millis();

                // If this is a command-initiated upload with expected size, check for completion
                if (instance._expectedUploadSize > 0 && (uint32_t)instance._bytesReceived >= instance._expectedUploadSize) {
                    Serial.println("Expected upload size reached. Finalizing upload...");
                    bool success = StorageManager::getInstance().finalizeBookWrite(instance._uploadTitle);
                    instance._isTransferring = false;
                    
                    // Respond to client
                    if (success) {
                        bleuart.println("UPLOAD_SUCCESS");
                    } else {
                        bleuart.println("UPLOAD_FAILED");
                    }
                    bleuart.flush();
                    delay(300);

                    if (instance._progressCallback) {
                        String status = success ? ("Book: " + (instance._uploadTitle.length() > 0 ? instance._uploadTitle : "Uploaded Book")) : "Error saving book";
                        instance._progressCallback(status, instance._bytesReceived, true);
                    }
                } else {
                    // Trigger progress updates
                    if (instance._progressCallback) {
                        String msg = "Received: " + String(instance._bytesReceived / 1024.0, 1) + " KB";
                        instance._progressCallback(msg, instance._bytesReceived, false);
                    }
                }
            }
        } else {
            // We are in Command Mode
            char c = bleuart.read();
            if (c == '\n') {
                instance._cmdBuffer.trim();
                if (instance._cmdBuffer.length() > 0) {
                    instance.processCommand(instance._cmdBuffer);
                }
                instance._cmdBuffer = "";
            } else if (c != '\r') {
                instance._cmdBuffer += c;
            }
        }
    }
}

void BleManager::processCommand(const String& cmd) {
    if (cmd.startsWith("CMD_LIST")) {
        // List books
        bleuart.println("LIST_START");
        bleuart.flush();
        delay(10);
        
        String books[MAX_BOOKS];
        int count = StorageManager::getInstance().listBooks(books, MAX_BOOKS);
        for (int i = 0; i < count; i++) {
            // Get file size
            Adafruit_LittleFS_Namespace::File f = StorageManager::getInstance().openBook(books[i], "r");
            uint32_t size = 0;
            if (f) {
                size = f.size();
                f.close();
            }
            bleuart.println(books[i] + ":" + String(size));
            bleuart.flush();
            delay(10);
        }
        bleuart.println("LIST_END");
        bleuart.flush();
    }
    else if (cmd.startsWith("CMD_DELETE:")) {
        String filename = cmd.substring(11);
        filename.trim();
        bool success = StorageManager::getInstance().deleteBook(filename);
        if (success) {
            bleuart.println("DELETE_SUCCESS:" + filename);
        } else {
            bleuart.println("DELETE_FAILED:" + filename);
        }
        bleuart.flush();
    }
    else if (cmd.startsWith("CMD_PAGE_NEXT")) {
        if (isSystemInReaderMode()) {
            bleuart.println("PAGE_SUCCESS");
            bleuart.flush();
            delay(300);
            bleRequestNextPage = true;
        } else {
            bleuart.println("PAGE_ERROR:not_in_reader_mode");
        }
    }
    else if (cmd.startsWith("CMD_PAGE_PREV")) {
        if (isSystemInReaderMode()) {
            bleuart.println("PAGE_SUCCESS");
            bleuart.flush();
            delay(300);
            bleRequestPrevPage = true;
        } else {
            bleuart.println("PAGE_ERROR:not_in_reader_mode");
        }
    }
    else if (cmd.startsWith("CMD_BTN_PREV")) {
        bleuart.println("BTN_SUCCESS");
        bleuart.flush();
        delay(300);
        ButtonManager::getInstance().injectPrevEvent(BTN_CLICK);
    }
    else if (cmd.startsWith("CMD_BTN_NEXT")) {
        bleuart.println("BTN_SUCCESS");
        bleuart.flush();
        delay(300);
        ButtonManager::getInstance().injectNextEvent(BTN_CLICK);
    }
    else if (cmd.startsWith("CMD_BTN_SELECT")) {
        bleuart.println("BTN_SUCCESS");
        bleuart.flush();
        delay(300);
        ButtonManager::getInstance().injectSelectEvent(BTN_CLICK);
    }
    else if (cmd.startsWith("CMD_BTN_SELECT_LONG")) {
        bleuart.println("BTN_SUCCESS");
        bleuart.flush();
        delay(300);
        ButtonManager::getInstance().injectSelectEvent(BTN_LONG_PRESS);
    }
    else if (cmd.startsWith("CMD_STATS")) {
        // Battery Stats
#ifdef SAADC_CH_PSELP_PSELP_VDDHDIV5
        float vbat = (analogReadVDDHDIV5() * 3.0f / 1024.0f) * 5.0f;
#else
        float vbat = (analogRead(PIN_BATTERY) * 3.3f / 1024.0f) * BATTERY_DIVIDER;
#endif
        int batPercent = map(vbat * 100, 330, 420, 0, 100);
        batPercent = constrain(batPercent, 0, 100);

        // RSSI using Nordic SoftDevice
        int8_t rssi = 0;
        uint8_t ch_index = 0;
        if (_connHandle != BLE_CONN_HANDLE_INVALID) {
            sd_ble_gap_rssi_get(_connHandle, &rssi, &ch_index);
        }

        // Storage Space
        uint32_t used = StorageManager::getInstance().getUsedSpace();
        uint32_t total = TOTAL_FS_SIZE;

        bleuart.println("STATS:vbat=" + String(vbat, 2) + "|percent=" + String(batPercent) + "|rssi=" + String(rssi) + "|used=" + String(used) + "|total=" + String(total));
        bleuart.flush();
    }
    else if (cmd.startsWith("CMD_UPLOAD:")) {
        // Format: CMD_UPLOAD:<size>:<title>
        int colonIdx = cmd.indexOf(':', 11);
        uint32_t size = 0;
        String title = "";
        if (colonIdx > 0) {
            size = cmd.substring(11, colonIdx).toInt();
            title = cmd.substring(colonIdx + 1);
        } else {
            size = cmd.substring(11).toInt();
        }
        title.trim();

        _isTransferring = true;
        _expectedUploadSize = size;
        _bytesReceived = 0;
        _uploadTitle = title;
        _lastDataTime = millis();

        StorageManager::getInstance().startNewBookWrite();
        bleuart.println("UPLOAD_READY");
        bleuart.flush();

        if (_progressCallback) {
            _progressCallback("Receiving: " + (title.length() > 0 ? title : "Book"), 0, false);
        }
    }
    else {
        // Legacy file upload fallback
        // Filter out non-printable ASCII noise and short stray strings to prevent spontaneous Upload Mode screen transitions
        bool isPrintable = true;
        for (size_t i = 0; i < cmd.length(); i++) {
            char c = cmd.charAt(i);
            if (c < 32 || c > 126) {
                isPrintable = false;
                break;
            }
        }

        if (cmd.length() >= 3 && isPrintable) {
            Serial.println("Legacy BLE stream detected. Initializing raw upload...");
            
            _isTransferring = true;
            _expectedUploadSize = 0; // run till quiet timeout
            _bytesReceived = 0;
            _uploadTitle = "";
            _lastDataTime = millis();

            StorageManager::getInstance().startNewBookWrite();
            
            // Write the first line we just received to the file with a newline
            String firstLine = cmd + "\n";
            StorageManager::getInstance().writeBookChunk((const uint8_t*)firstLine.c_str(), firstLine.length());
            _bytesReceived += firstLine.length();

            if (_progressCallback) {
                _progressCallback("Receiving book...", _bytesReceived, false);
            }
        } else {
            Serial.print("Ignoring invalid command/garbage bytes: '");
            Serial.print(cmd);
            Serial.println("'");
        }
    }
}

// Central role methods
void BleManager::startScanning() {
    if (!_isScanning && !_centralConnected) {
        Bluefruit.Scanner.start(0); // 0 = scan forever
        _isScanning = true;
        Serial.println("Central scanning started.");
    }
}

void BleManager::stopScanning() {
    if (_isScanning) {
        Bluefruit.Scanner.stop();
        _isScanning = false;
        Serial.println("Central scanning stopped.");
    }
}

bool BleManager::isCentralConnected() const {
    return _centralConnected;
}

static void sendNUSNotification(BLEUart& uart, const String& msg) {
    int len = msg.length();
    int offset = 0;
    uint32_t startAttempt = millis();
    while (offset < len && (millis() - startAttempt < 2000)) {
        int chunk = min(20, len - offset);
        int written = uart.write((const uint8_t*)(msg.c_str() + offset), chunk);
        if (written > 0) {
            offset += written;
            startAttempt = millis(); // Reset timeout on progress
            delay(50); // 50ms pacing delay between packets
        } else {
            delay(10); // Wait for BLE TX buffer to free up
        }
    }
}

static void sendNUSClientWrite(BLEClientUart& client, const String& msg) {
    int len = msg.length();
    int offset = 0;
    uint32_t startAttempt = millis();
    while (offset < len && (millis() - startAttempt < 2000)) {
        int chunk = min(20, len - offset);
        int written = client.write((const uint8_t*)(msg.c_str() + offset), chunk);
        if (written > 0) {
            offset += written;
            startAttempt = millis(); // Reset timeout on progress
            delay(50); // 50ms pacing delay
        } else {
            delay(10); // Wait for BLE TX buffer to free up
        }
    }
}

bool BleManager::requestBookList(String books[], int maxBooks, int& count) {
    bool isCentral = _centralConnected;
    bool isPeripheral = _connected;
    Serial.print("[BLE Request Debug] requestBookList: isCentral=");
    Serial.print(isCentral);
    Serial.print(", isPeripheral=");
    Serial.println(isPeripheral);
    
    if (!isCentral && !isPeripheral) return false;
    
    count = 0;
    
    // Clear RX buffer
    if (isCentral) {
        while (_clientUart.available()) _clientUart.read();
        sendNUSClientWrite(_clientUart, "REQ_LIST\n");
    } else {
        while (bleuart.available()) bleuart.read();
        sendNUSNotification(bleuart, "REQ_LIST\n");
    }
    Serial.println("[BLE Request Debug] Sent: REQ_LIST");
    
    // Read response with timeout
    uint32_t startTime = millis();
    String line = "";
    bool listStarted = false;
    
    while (millis() - startTime < 5000) { // 5-second timeout
        int avail = isCentral ? _clientUart.available() : bleuart.available();
        if (avail) {
            char c = isCentral ? _clientUart.read() : bleuart.read();
            if (c == '\n') {
                line.trim();
                if (line == "LIST_START") {
                    listStarted = true;
                } else if (line == "LIST_END") {
                    Serial.println("[BLE Request Debug] Received LIST_END successfully.");
                    return true;
                } else if (listStarted && line.startsWith("BOOK:")) {
                    // Parse BOOK:filename:size
                    String bookInfo = line.substring(5);
                    int colonIdx = bookInfo.indexOf(':');
                    String filename = (colonIdx > 0) ? bookInfo.substring(0, colonIdx) : bookInfo;
                    if (count < maxBooks) {
                        books[count++] = filename;
                    }
                }
                line = "";
            } else if (c != '\r') {
                line += c;
            }
        }
        yield();
    }
    
    Serial.println("[BLE Request Debug] requestBookList timed out.");
    return false; // Timeout
}

uint32_t BleManager::requestBookSize(const String& filename) {
    bool isCentral = _centralConnected;
    bool isPeripheral = _connected;
    Serial.print("[BLE Request Debug] requestBookSize: filename=");
    Serial.print(filename);
    Serial.print(", isCentral=");
    Serial.print(isCentral);
    Serial.print(", isPeripheral=");
    Serial.println(isPeripheral);
    
    if (!isCentral && !isPeripheral) return 0;
    
    // Clear RX buffer
    if (isCentral) {
        while (_clientUart.available()) _clientUart.read();
        sendNUSClientWrite(_clientUart, "REQ_SIZE:" + filename + "\n");
    } else {
        while (bleuart.available()) bleuart.read();
        sendNUSNotification(bleuart, "REQ_SIZE:" + filename + "\n");
    }
    Serial.println("[BLE Request Debug] Sent: REQ_SIZE:" + filename);
    
    // Read response with timeout: SIZE:<bytes>
    uint32_t startTime = millis();
    String line = "";
    
    while (millis() - startTime < 3000) { // 3-second timeout
        int avail = isCentral ? _clientUart.available() : bleuart.available();
        if (avail) {
            char c = isCentral ? _clientUart.read() : bleuart.read();
            if (c == '\n') {
                line.trim();
                if (line.startsWith("SIZE:")) {
                    uint32_t size = line.substring(5).toInt();
                    Serial.print("[BLE Request Debug] Received SIZE: ");
                    Serial.println(size);
                    return size;
                }
                line = "";
            } else if (c != '\r') {
                line += c;
            }
        }
        yield();
    }
    
    Serial.println("[BLE Request Debug] requestBookSize timed out.");
    return 0; // Timeout
}

int BleManager::requestBookText(const String& filename, uint32_t offset, char* buffer, int maxLen) {
    bool isCentral = _centralConnected;
    bool isPeripheral = _connected;
    Serial.print("[BLE Request Debug] requestBookText: filename=");
    Serial.print(filename);
    Serial.print(", offset=");
    Serial.print(offset);
    Serial.print(", isCentral=");
    Serial.print(isCentral);
    Serial.print(", isPeripheral=");
    Serial.println(isPeripheral);
    
    if (!isCentral && !isPeripheral) return 0;
    
    // Clear RX buffer
    if (isCentral) {
        while (_clientUart.available()) _clientUart.read();
        sendNUSClientWrite(_clientUart, "REQ_TEXT:" + filename + ":" + String(offset) + ":" + String(maxLen) + "\n");
    } else {
        while (bleuart.available()) bleuart.read();
        sendNUSNotification(bleuart, "REQ_TEXT:" + filename + ":" + String(offset) + ":" + String(maxLen) + "\n");
    }
    Serial.print("[BLE Request Debug] Sent: REQ_TEXT:");
    Serial.print(filename);
    Serial.print(":");
    Serial.println(offset);
    
    // Read response header: TEXT_START:<length>
    uint32_t startTime = millis();
    String header = "";
    int expectedBytes = -1;
    
    while (millis() - startTime < 5000) { // 5-second timeout
        int avail = isCentral ? _clientUart.available() : bleuart.available();
        if (avail) {
            char c = isCentral ? _clientUart.read() : bleuart.read();
            if (c == '\n') {
                header.trim();
                if (header.startsWith("TEXT_START:")) {
                    expectedBytes = header.substring(11).toInt();
                    break;
                }
                header = "";
            } else if (c != '\r') {
                header += c;
            }
        }
        yield();
    }
    
    if (expectedBytes < 0) {
        Serial.println("Timeout waiting for TEXT_START header.");
        return 0; // Header timeout or parse failure
    }
    
    Serial.print("[BLE Request Debug] Received TEXT_START: ");
    Serial.println(expectedBytes);
    
    if (expectedBytes > maxLen) {
        expectedBytes = maxLen; // Limit to buffer size
    }
    
    // Read the raw text bytes
    int bytesRead = 0;
    startTime = millis();
    
    while (bytesRead < expectedBytes && (millis() - startTime < 5000)) {
        int avail = isCentral ? _clientUart.available() : bleuart.available();
        if (avail) {
            buffer[bytesRead++] = isCentral ? _clientUart.read() : bleuart.read();
            startTime = millis(); // Reset timeout on byte received
        }
        yield();
    }
    
    if (bytesRead < expectedBytes) {
        Serial.print("Timeout reading raw text. Read ");
        Serial.print(bytesRead);
        Serial.print(" of ");
        Serial.println(expectedBytes);
    } else {
        Serial.print("[BLE Request Debug] Successfully read ");
        Serial.print(bytesRead);
        Serial.println(" bytes of book text.");
    }
    
    return bytesRead;
}

// Central callbacks
void BleManager::scan_callback(ble_gap_evt_adv_report_t* report) {
    char name[32] = { 0 };
    
    Bluefruit.Scanner.parseReportByType(report, BLE_GAP_AD_TYPE_COMPLETE_LOCAL_NAME, (uint8_t*)name, sizeof(name));
    if (!name[0]) {
        Bluefruit.Scanner.parseReportByType(report, BLE_GAP_AD_TYPE_SHORT_LOCAL_NAME, (uint8_t*)name, sizeof(name));
    }

    // Print all discovered devices to serial log for debugging
    Serial.print("[Scan Debug] Found device: '");
    Serial.print(name[0] ? name : "[No Name]");
    Serial.print("' RSSI: ");
    Serial.print(report->rssi);
    Serial.print(" MAC: ");
    for (int i = 0; i < 6; i++) {
        Serial.print(report->peer_addr.addr[5-i], HEX);
        if (i < 5) Serial.print(":");
    }
    
    // Print if it has our NUS UUID
    bool hasNus = Bluefruit.Scanner.checkReportForUuid(report, BLEUuid("6e400001-b5a3-f393-e0a9-e50e24dcca9e"));
    if (hasNus) {
        Serial.print(" (Has NUS UUID!)");
    }
    Serial.println();

    bool isMatch = false;
    if (name[0] && String(name) == "EpdBookServer") {
        isMatch = true;
    }
    if (!isMatch && hasNus) {
        isMatch = true;
    }
    
    if (isMatch) {
        Serial.print("Found BLE Book Server: ");
        Serial.println(name[0] ? name : "[NUS Service]");
        Bluefruit.Central.connect(report);
    }
}

void BleManager::central_connect_callback(uint16_t conn_handle) {
    Serial.println("Central Connected to PC.");
    BleManager& instance = BleManager::getInstance();
    
    if (instance._clientUart.discover(conn_handle)) {
        Serial.println("NUS service discovered on PC.");
        instance._centralConnected = true;
        instance._centralConnHandle = conn_handle;
        instance._clientUart.enableTXD(); // Enable notifications from server
    } else {
        Serial.println("Failed to discover NUS service on PC.");
        Bluefruit.disconnect(conn_handle);
    }
}

void BleManager::central_disconnect_callback(uint16_t conn_handle, uint8_t reason) {
    (void) conn_handle;
    (void) reason;
    Serial.println("Central Disconnected from PC.");
    BleManager& instance = BleManager::getInstance();
    instance._centralConnected = false;
    instance._centralConnHandle = BLE_CONN_HANDLE_INVALID;
    instance._isScanning = false;
}
