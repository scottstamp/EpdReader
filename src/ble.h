#ifndef BLE_H
#define BLE_H

#include "config.h"

typedef void (*ble_status_cb_t)(bool connected);
typedef void (*ble_data_cb_t)(const uint8_t *data, uint16_t len);

int ble_init(ble_status_cb_t status_cb, ble_data_cb_t data_cb);
void ble_start_advertising(void);
void ble_stop_advertising(void);
bool ble_is_connected(void);
void ble_update_battery_level(uint8_t level);
int ble_send_data(const uint8_t *data, uint16_t len);

#endif // BLE_H
