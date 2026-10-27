#ifndef BLE_UPLOAD_H
#define BLE_UPLOAD_H

#include "config.h"

// Book upload over Bluetooth (protocol in upload_proto.h). While active, the
// upload worker thread owns the SD card; the caller must not touch it until
// ble_upload_stop() has returned.

typedef struct {
    bool connected;
    bool receiving;
    char name[64];
    uint32_t size;
    uint32_t received;     // bytes written to the card so far
    uint32_t result_seq;   // bumps each time a transfer ends (saved or failed)
    bool result_ok;
    const char *result_msg;
    char result_name[64];
} BleUploadStatus;

// Enables Bluetooth, starts advertising and accepts uploads. buf becomes the
// receive buffer (= flow-control window, ideally >= 1 KB) and stays in use
// until ble_upload_stop() returns.
int ble_upload_start(uint8_t *buf, size_t size);
// Aborts any transfer (deleting the partial file), disconnects, stops
// advertising and waits until the worker no longer touches the SD card or buf
void ble_upload_stop(void);
void ble_upload_get_status(BleUploadStatus *st);

#endif // BLE_UPLOAD_H
