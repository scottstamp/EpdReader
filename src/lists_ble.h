#ifndef LISTS_BLE_H
#define LISTS_BLE_H

// Lists sync over Bluetooth LE: a GATT service separate from book upload.
//
//   Service        8d3a0001-6f1b-4c1e-9b8a-2f0e7c5d4a10
//   Characteristic 8d3a0002-6f1b-4c1e-9b8a-2f0e7c5d4a10  (write with response + notify)
//
// Client -> device (little-endian):
//   GET         10
//   PUT_BEGIN   11 | size u16 | base_rev u32 | crc32 u32
//   PUT_DATA    12 | offset u16 | bytes          (in order, each write fits the ATT MTU)
//   PUT_COMMIT  13
// Device -> client (notifications):
//   DOC_BEGIN   90 | size u16 | rev u32 | crc32 u32 | max_write u16
//   DOC_DATA    91 | offset u16 | bytes
//   DOC_END     92
//   PUT_OK      93 | rev u32
//   PUT_ERR     94 | code u8 | rev u32           (rev = the device's current revision)
//   CHANGED     95 | rev u32                     (lists edited on the device)
//
// The document is the text format in lists_doc.h; crc32 is zlib's. A client GETs first
// (DOC_BEGIN tells it the largest PUT_DATA write the link carries), edits, then PUTs
// with the revision it started from. If the device's lists changed in between, PUT_ERR
// code 3 (conflict) comes back and the client should GET again.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum lists_ble_err {
    LISTS_BLE_ERR_BAD_FRAME = 1,
    LISTS_BLE_ERR_TOO_BIG = 2,
    LISTS_BLE_ERR_CONFLICT = 3,
    LISTS_BLE_ERR_CRC = 4,
    LISTS_BLE_ERR_INVALID = 5,
    LISTS_BLE_ERR_STORAGE = 6,
    LISTS_BLE_ERR_BUSY = 7,
};

// Starts advertising; PUT data is staged in buf (lent until lists_ble_stop()).
int lists_ble_start(uint8_t *buf, size_t size);
void lists_ble_stop(void);
bool lists_ble_connected(void);

// Main thread, called regularly while the lists screens are open: answers GETs and
// applies committed PUTs. Returns true if the lists were replaced (redraw).
bool lists_ble_poll(void);
// Tell a connected client the lists changed on the device
void lists_ble_notify_changed(void);

#endif // LISTS_BLE_H
