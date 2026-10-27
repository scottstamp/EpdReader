#include "ble.h"
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/services/bas.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ble, LOG_LEVEL_INF);

static struct bt_conn *current_conn = NULL;
static ble_status_cb_t user_status_cb = NULL;
static ble_data_cb_t user_data_cb = NULL;
static bool bt_ready = false;
// Advertising is wanted (upload screen open); resumed after each disconnect
static volatile bool adv_wanted = false;

// Nordic UART Service (NUS) UUIDs
#define BT_UUID_NUS_VAL BT_UUID_128_ENCODE(0x6e400001, 0xb5a3, 0xf393, 0xe0a9, 0xe50e24dcca9e)
#define BT_UUID_NUS_RX_VAL BT_UUID_128_ENCODE(0x6e400002, 0xb5a3, 0xf393, 0xe0a9, 0xe50e24dcca9e)
#define BT_UUID_NUS_TX_VAL BT_UUID_128_ENCODE(0x6e400003, 0xb5a3, 0xf393, 0xe0a9, 0xe50e24dcca9e)

static struct bt_uuid_128 nus_uuid = BT_UUID_INIT_128(BT_UUID_NUS_VAL);
static struct bt_uuid_128 nus_rx_uuid = BT_UUID_INIT_128(BT_UUID_NUS_RX_VAL);
static struct bt_uuid_128 nus_tx_uuid = BT_UUID_INIT_128(BT_UUID_NUS_TX_VAL);

static ssize_t nus_rx_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                            const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    // Each write is one upload frame; long (prepared) writes are not part of it
    if (offset != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    if (user_data_cb && len > 0) {
        user_data_cb((const uint8_t *)buf, len);
    }
    return len;
}

BT_GATT_SERVICE_DEFINE(nus_svc,
    BT_GATT_PRIMARY_SERVICE(&nus_uuid),
    BT_GATT_CHARACTERISTIC(&nus_rx_uuid.uuid, BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_WRITE, NULL, nus_rx_write, NULL),
    BT_GATT_CHARACTERISTIC(&nus_tx_uuid.uuid, BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_NONE, NULL, NULL, NULL),
    BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

static void connected(struct bt_conn *conn, uint8_t err) {
    if (err) {
        LOG_ERR("BLE Connection failed (err 0x%02x)", err);
        return;
    }
    LOG_INF("BLE Connected.");
    current_conn = bt_conn_ref(conn);
    if (user_status_cb) {
        user_status_cb(true);
    }
}

static void disconnected(struct bt_conn *conn, uint8_t reason) {
    LOG_INF("BLE Disconnected (reason 0x%02x).", reason);
    if (current_conn) {
        bt_conn_unref(current_conn);
        current_conn = NULL;
    }
    if (user_status_cb) {
        user_status_cb(false);
    }
}

static void adv_start(void);

static void adv_restart_handler(struct k_work *work) {
    if (adv_wanted) {
        adv_start();
    }
}

static K_WORK_DEFINE(adv_restart_work, adv_restart_handler);

// The connection object is free again, so connectable advertising can resume
static void recycled(void) {
    if (adv_wanted) {
        k_work_submit(&adv_restart_work);
    }
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
    .recycled = recycled,
};

static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_VAL),
};

static const struct bt_data sd[] = {
    BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

int ble_init(ble_status_cb_t status_cb, ble_data_cb_t data_cb) {
    user_status_cb = status_cb;
    user_data_cb = data_cb;
    if (bt_ready) {
        return 0;
    }

    int err = bt_enable(NULL);
    if (err == -EALREADY) {
        err = 0;
    }
    if (err) {
        LOG_ERR("Bluetooth init failed (err %d)", err);
        return err;
    }

    bt_ready = true;
    LOG_INF("Bluetooth initialized successfully.");
    return 0;
}

static void adv_start(void) {
    int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
    if (err) {
        LOG_ERR("Advertising failed to start (err %d)", err);
    } else {
        LOG_INF("Advertising started.");
    }
}

void ble_start_advertising(void) {
    adv_wanted = true;
    if (!current_conn) {
        adv_start(); // otherwise recycled() resumes it once the link is gone
    }
}

void ble_stop_advertising(void) {
    struct k_work_sync sync;
    adv_wanted = false;
    k_work_cancel_sync(&adv_restart_work, &sync);
    bt_le_adv_stop();
    LOG_INF("Advertising stopped.");
}

bool ble_is_connected(void) {
    return (current_conn != NULL);
}

void ble_disconnect(void) {
    struct bt_conn *conn = current_conn ? bt_conn_ref(current_conn) : NULL;
    if (conn) {
        bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        bt_conn_unref(conn);
    }
}

uint16_t ble_get_mtu(void) {
    struct bt_conn *conn = current_conn ? bt_conn_ref(current_conn) : NULL;
    uint16_t mtu = 23; // ATT default before the exchange
    if (conn) {
        mtu = bt_gatt_get_mtu(conn);
        bt_conn_unref(conn);
    }
    return mtu;
}

void ble_update_battery_level(uint8_t level) {
    bt_bas_set_battery_level(level);
}

int ble_send_data(const uint8_t *data, uint16_t len) {
    if (!current_conn) return -ENOTCONN;
    return bt_gatt_notify(current_conn, &nus_svc.attrs[3], data, len);
}
