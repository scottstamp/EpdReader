#ifndef BLE_MANAGER_H
#define BLE_MANAGER_H

#include <Arduino.h>
#include <bluefruit.h>

typedef void (*BleStateCallback)(bool connected);
typedef void (*BleProgressCallback)(const String& status, int bytesReceived, bool finished);

class BleManager {
public:
    static BleManager& getInstance();

    void begin(BleStateCallback stateCb, BleProgressCallback progressCb);
    void startAdvertising();
    void stopAdvertising();
    bool isConnected() const;
    void update(); // Poll for timeout

    void resetTransferState();
    void processCommand(const String& cmd);

    // Central role methods
    void startScanning();
    void stopScanning();
    bool isCentralConnected() const;
    bool requestBookList(String books[], int maxBooks, int& count);
    uint32_t requestBookSize(const String& filename);
    int requestBookText(const String& filename, uint32_t offset, char* buffer, int maxLen);

private:
    BleManager();
    BleManager(const BleManager&) = delete;
    BleManager& operator=(const BleManager&) = delete;

    // Internal BLE Callbacks
    static void connect_callback(uint16_t conn_handle);
    static void disconnect_callback(uint16_t conn_handle, uint8_t reason);
    static void rx_callback(uint16_t conn_handle);

    // Central callbacks
    static void scan_callback(ble_gap_evt_adv_report_t* report);
    static void central_connect_callback(uint16_t conn_handle);
    static void central_disconnect_callback(uint16_t conn_handle, uint8_t reason);

    BleStateCallback _stateCallback;
    BleProgressCallback _progressCallback;

    bool _connected;
    bool _isTransferring;
    uint32_t _lastDataTime;
    int _bytesReceived;

    uint16_t _connHandle;
    uint32_t _expectedUploadSize;
    String _uploadTitle;
    String _cmdBuffer;

    // Central status variables
    BLEClientUart _clientUart;
    bool _centralConnected;
    bool _isScanning;
    uint16_t _centralConnHandle;

    static BleManager* _instance;
};

#endif // BLE_MANAGER_H
