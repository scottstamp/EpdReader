#include "lists_ble.h"
#include "lists.h"
#include "ble.h"
#include "upload_proto.h"
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(lists_ble, LOG_LEVEL_INF);

#define OP_GET        0x10
#define OP_PUT_BEGIN  0x11
#define OP_PUT_DATA   0x12
#define OP_PUT_COMMIT 0x13
#define EV_DOC_BEGIN  0x90
#define EV_DOC_DATA   0x91
#define EV_DOC_END    0x92
#define EV_PUT_OK     0x93
#define EV_PUT_ERR    0x94
#define EV_CHANGED    0x95

#define ATT_HDR 3   // notification / write header
#define DATA_HDR 3  // opcode + offset u16

#define BT_UUID_LISTS_VAL      BT_UUID_128_ENCODE(0x8d3a0001, 0x6f1b, 0x4c1e, 0x9b8a, 0x2f0e7c5d4a10)
#define BT_UUID_LISTS_DATA_VAL BT_UUID_128_ENCODE(0x8d3a0002, 0x6f1b, 0x4c1e, 0x9b8a, 0x2f0e7c5d4a10)

static struct bt_uuid_128 lists_uuid = BT_UUID_INIT_128(BT_UUID_LISTS_VAL);
static struct bt_uuid_128 lists_data_uuid = BT_UUID_INIT_128(BT_UUID_LISTS_DATA_VAL);

// Receive-side state, written from the Bluetooth RX thread
static K_MUTEX_DEFINE(rx_lock);
static struct {
    bool active;
    uint8_t *stage;
    size_t stage_cap;
    bool receiving;
    bool committed;      // a complete PUT waits for the main thread
    bool get_requested;
    uint8_t err;         // PUT_ERR to send, 0 = none
    uint16_t size;
    uint16_t got;
    uint32_t base_rev;
    uint32_t crc;
} rx;

static uint16_t get_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

// Called locked
static void rx_fail(uint8_t err) {
    rx.receiving = false;
    if (!rx.err) rx.err = err;
}

static ssize_t lists_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    if (offset != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    const uint8_t *f = buf;
    if (len == 0) {
        return len;
    }

    k_mutex_lock(&rx_lock, K_FOREVER);
    if (!rx.active) {
        // Only served while the lists screen is open
    } else if (f[0] == OP_GET && len == 1) {
        rx.get_requested = true;
    } else if (f[0] == OP_PUT_BEGIN && len == 11) {
        if (rx.committed) {
            rx_fail(LISTS_BLE_ERR_BUSY);
        } else {
            rx.size = get_u16(f + 1);
            rx.base_rev = get_u32(f + 3);
            rx.crc = get_u32(f + 7);
            rx.got = 0;
            rx.receiving = rx.size <= MIN(rx.stage_cap, LISTS_PUT_MAX);
            if (!rx.receiving) rx_fail(LISTS_BLE_ERR_TOO_BIG);
        }
    } else if (f[0] == OP_PUT_DATA && len > DATA_HDR) {
        uint16_t off = get_u16(f + 1);
        uint16_t n = len - DATA_HDR;
        if (!rx.receiving || off != rx.got || n > rx.size - rx.got) {
            rx_fail(LISTS_BLE_ERR_BAD_FRAME);
        } else {
            memcpy(rx.stage + off, f + DATA_HDR, n);
            rx.got += n;
        }
    } else if (f[0] == OP_PUT_COMMIT && len == 1) {
        if (!rx.receiving || rx.got != rx.size) {
            rx_fail(LISTS_BLE_ERR_BAD_FRAME);
        } else if (up_crc32(0, rx.stage, rx.size) != rx.crc) {
            rx_fail(LISTS_BLE_ERR_CRC);
        } else {
            rx.receiving = false;
            rx.committed = true;
        }
    } else {
        rx_fail(LISTS_BLE_ERR_BAD_FRAME);
    }
    k_mutex_unlock(&rx_lock);
    return len;
}

BT_GATT_SERVICE_DEFINE(lists_svc,
    BT_GATT_PRIMARY_SERVICE(&lists_uuid),
    BT_GATT_CHARACTERISTIC(&lists_data_uuid.uuid, BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_WRITE, NULL, lists_write, NULL),
    BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

static void notify(const uint8_t *data, uint16_t len) {
    // Notification buffers are shared with the stack; wait briefly for one
    for (int tries = 0; tries < 50; tries++) {
        int err = bt_gatt_notify(NULL, &lists_svc.attrs[2], data, len);
        if (err != -ENOMEM && err != -ENOBUFS && err != -EAGAIN) {
            return;
        }
        k_msleep(10);
    }
}

static void send_doc(void) {
    const ListsDoc *doc = lists_get();
    uint16_t mtu = ble_get_mtu();
    uint16_t chunk = (mtu > ATT_HDR + DATA_HDR) ? mtu - ATT_HDR - DATA_HDR : 1;
    uint8_t frame[247];
    chunk = MIN(chunk, sizeof(frame) - DATA_HDR);

    frame[0] = EV_DOC_BEGIN;
    put_u16(frame + 1, doc->len);
    put_u32(frame + 3, doc->rev);
    put_u32(frame + 7, up_crc32(0, (const uint8_t *)doc->text, doc->len));
    put_u16(frame + 11, chunk);  // max_write: the same limit applies to the client's writes
    notify(frame, 13);

    for (uint16_t off = 0; off < doc->len; off += chunk) {
        uint16_t n = MIN(chunk, doc->len - off);
        frame[0] = EV_DOC_DATA;
        put_u16(frame + 1, off);
        memcpy(frame + DATA_HDR, doc->text + off, n);
        notify(frame, DATA_HDR + n);
    }
    frame[0] = EV_DOC_END;
    notify(frame, 1);
}

static void send_put_result(uint8_t err) {
    uint8_t frame[6];
    uint32_t rev = lists_get()->rev;
    if (err) {
        frame[0] = EV_PUT_ERR;
        frame[1] = err;
        put_u32(frame + 2, rev);
        notify(frame, 6);
    } else {
        frame[0] = EV_PUT_OK;
        put_u32(frame + 1, rev);
        notify(frame, 5);
    }
}

static uint8_t to_ble_err(int lists_err) {
    switch (lists_err) {
        case LISTS_OK: return 0;
        case LISTS_ERR_CONFLICT: return LISTS_BLE_ERR_CONFLICT;
        case LISTS_ERR_STORAGE: return LISTS_BLE_ERR_STORAGE;
        case LISTS_ERR_TOO_BIG: return LISTS_BLE_ERR_TOO_BIG;
        default: return LISTS_BLE_ERR_INVALID;
    }
}

bool lists_ble_poll(void) {
    k_mutex_lock(&rx_lock, K_FOREVER);
    bool get = rx.get_requested;
    bool committed = rx.committed;
    uint8_t err = rx.err;
    rx.get_requested = false;
    rx.err = 0;
    k_mutex_unlock(&rx_lock);

    if (err) {
        send_put_result(err);
    }

    bool replaced = false;
    if (committed) {
        // The RX side leaves the staging buffer alone until committed is cleared
        int res = lists_replace((const char *)rx.stage, rx.size, rx.base_rev);
        replaced = (res == LISTS_OK);
        send_put_result(to_ble_err(res));
        k_mutex_lock(&rx_lock, K_FOREVER);
        rx.committed = false;
        k_mutex_unlock(&rx_lock);
    }

    if (get) {
        send_doc();
    }
    return replaced;
}

void lists_ble_notify_changed(void) {
    if (!ble_is_connected()) return;
    uint8_t frame[5];
    frame[0] = EV_CHANGED;
    put_u32(frame + 1, lists_get()->rev);
    notify(frame, sizeof(frame));
}

int lists_ble_start(uint8_t *buf, size_t size) {
    int err = ble_init(NULL, NULL);
    if (err) {
        return err;
    }
    k_mutex_lock(&rx_lock, K_FOREVER);
    memset(&rx, 0, sizeof(rx));
    rx.stage = buf;
    rx.stage_cap = size;
    rx.active = true;
    k_mutex_unlock(&rx_lock);
    ble_start_advertising();
    return 0;
}

void lists_ble_stop(void) {
    k_mutex_lock(&rx_lock, K_FOREVER);
    rx.active = false;
    rx.receiving = false;
    rx.committed = false;
    k_mutex_unlock(&rx_lock);
    ble_stop_advertising();
    ble_disconnect();
}

bool lists_ble_connected(void) {
    return ble_is_connected();
}
