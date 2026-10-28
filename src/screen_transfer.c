// Getting books onto the SD card: USB drive mode and Bluetooth upload
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_msc.h>
#include <stdio.h>
#include "ble_upload.h"
#include "book.h"
#include "display.h"
#include "screens.h"
#include "storage.h"

LOG_MODULE_REGISTER(transfer, LOG_LEVEL_INF);

// --- Transfer menu ---

typedef enum {
    TRANSFER_USB,
    TRANSFER_BLUETOOTH,
    TRANSFER_BACK,
    TRANSFER_COUNT
} TransferItem;

static int selected = 0;

static void transfer_enter(SystemState from) {
    (void)from;
    selected = 0;
}

static void transfer_render(void) {
    const char *options[TRANSFER_COUNT] = {
        [TRANSFER_USB] = "USB drive (SD card)",
        [TRANSFER_BLUETOOTH] = "Bluetooth upload",
        [TRANSFER_BACK] = "[Back]",
    };
    display_draw_menu("Transfer", options, TRANSFER_COUNT, selected);
}

static void transfer_step(int delta) {
    selected = wrap_index(selected, delta, TRANSFER_COUNT);
    app_redraw();
}

static void transfer_select(void) {
    switch ((TransferItem)selected) {
        case TRANSFER_USB:       app_go(STATE_USB_STORAGE); break;
        case TRANSFER_BLUETOOTH: app_go(STATE_BLE_UPLOAD); break;
        default:                 app_go(STATE_MENU); break;
    }
}

const Screen screen_transfer = {
    .enter = transfer_enter,
    .render = transfer_render,
    .step = transfer_step,
    .select = transfer_select,
};

static void back_home(void) {
    app_go(STATE_MENU);
}

// --- Bluetooth upload ---
// The upload worker owns the SD card from enter until leave returns.

#define BLE_UPLOAD_POLL_MS 500

static bool ble_active = false;
static uint32_t ble_seq_on_entry = 0;  // only results from this visit are shown
static uint32_t ble_ui_key = 0;        // what the screen shows; redraw when it changes

static void ble_enter(SystemState from) {
    (void)from;
    book_flush_progress();
    BleUploadStatus st;
    ble_upload_get_status(&st);
    ble_seq_on_entry = st.result_seq;
    // The reader's scratch buffer is idle on this screen; lending it as the receive
    // window saves a dedicated 3 KB buffer. ble_leave() takes it back.
    size_t size;
    uint8_t *buf = book_scratch(&size);
    ble_active = (ble_upload_start(buf, size) == 0);
}

static void ble_leave(SystemState to) {
    (void)to;
    if (ble_active) {
        ble_upload_stop();
        ble_active = false;
        // An upload may have replaced the open book; drop its in-RAM index so it reloads
        storage_forget_cached_book();
    }
}

static int upload_percent(const BleUploadStatus *st) {
    return st->size ? (int)(((uint64_t)st->received * 100) / st->size) : 0;
}

// Screen content in one number: e-paper redraws are slow, so progress moves in 10% steps
static uint32_t upload_ui_key(const BleUploadStatus *st) {
    if (!ble_active) return 1;
    uint32_t conn = st->connected ? 1 : 0;
    if (st->receiving) return 0x100 | (uint32_t)(upload_percent(st) / 10);
    if (st->result_seq != ble_seq_on_entry) return 0x200 | conn | ((st->result_seq & 0xFFFF) << 16);
    return 0x300 | conn;
}

static void ble_render(void) {
    BleUploadStatus st;
    ble_upload_get_status(&st);
    ble_ui_key = upload_ui_key(&st);

    char title[96];
    if (!ble_active) {
        display_draw_message("Bluetooth Upload", "Bluetooth unavailable", true);
    } else if (st.receiving) {
        int pct = upload_percent(&st);
        snprintf(title, sizeof(title), "Receiving %.40s  %d%%", st.name, pct);
        display_draw_progress(title, pct);
    } else if (st.result_seq != ble_seq_on_entry) {
        if (st.result_ok) {
            display_draw_message(st.connected ? "Saved. Send another or press Select" : "Saved. Press Select to finish",
                                 st.result_name, false);
        } else {
            snprintf(title, sizeof(title), "Upload failed: %s", st.result_msg);
            display_draw_message(title, st.result_name, true);
        }
    } else {
        display_draw_message("Bluetooth: " CONFIG_BT_DEVICE_NAME,
                             st.connected ? "Connected" : "Waiting for connection", false);
    }
}

static void ble_poll(void) {
    BleUploadStatus st;
    ble_upload_get_status(&st);
    if (st.connected) {
        app_keep_awake(); // no auto-sleep while a computer is linked
    }
    if (upload_ui_key(&st) != ble_ui_key) {
        app_redraw();
    }
}

const Screen screen_ble_upload = {
    .enter = ble_enter,
    .leave = ble_leave,
    .render = ble_render,
    .select = back_home,
    .poll = ble_poll,
    .poll_ms = BLE_UPLOAD_POLL_MS,
    .long_press_sleeps = true,
};

// --- USB drive mode ---
// The serial console is always available; the SD card is only offered as a USB drive in
// this mode, while its FAT volume is unmounted here. Otherwise the computer and the
// firmware could both write the filesystem and corrupt it.

static bool usb_msc_active = false;

#if defined(CONFIG_USB_DEVICE_STACK_NEXT)
USBD_DEFINE_MSC_LUN(sd, "SD", "NiceKeyboards", "EPD Reader", "1.00");

USBD_DEVICE_DEFINE(epd_usbd,
                   DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
                   0x2FE3, 0x0004);
USBD_DESC_LANG_DEFINE(epd_lang);
USBD_DESC_MANUFACTURER_DEFINE(epd_mfr, "NiceKeyboards");
USBD_DESC_PRODUCT_DEFINE(epd_product, "nRF EPD Reader");
USBD_DESC_CONFIG_DEFINE(epd_fs_cfg_desc, "FS Configuration");
USBD_CONFIGURATION_DEFINE(epd_fs_config, 0, 125, &epd_fs_cfg_desc);

static int usb_start(bool with_msc) {
    int err = usbd_add_descriptor(&epd_usbd, &epd_lang);
    if (!err) err = usbd_add_descriptor(&epd_usbd, &epd_mfr);
    if (!err) err = usbd_add_descriptor(&epd_usbd, &epd_product);
    if (!err) err = usbd_add_configuration(&epd_usbd, USBD_SPEED_FS, &epd_fs_config);
    if (!err) err = usbd_register_class(&epd_usbd, "cdc_acm_0", USBD_SPEED_FS, 1);
    if (!err && with_msc) err = usbd_register_class(&epd_usbd, "msc_0", USBD_SPEED_FS, 1);
    if (err) {
        LOG_ERR("Failed to configure USB device (err %d)", err);
        return err;
    }
    usbd_device_set_code_triple(&epd_usbd, USBD_SPEED_FS, USB_BCC_MISCELLANEOUS, 0x02, 0x01);
    err = usbd_init(&epd_usbd);
    if (!err) err = usbd_enable(&epd_usbd);
    if (err) {
        LOG_ERR("Failed to start USB device (err %d)", err);
    }
    return err;
}

// Re-enumerates with or without the mass storage class
static int usb_restart(bool with_msc) {
    usbd_disable(&epd_usbd);
    usbd_shutdown(&epd_usbd);
    return usb_start(with_msc);
}
#endif

void usb_init(void) {
#if defined(CONFIG_USB_DEVICE_STACK_NEXT)
    usb_start(false);
#endif
}

static void usb_enter(SystemState from) {
    (void)from;
    book_flush_progress();
    user_settings_save();
#if defined(CONFIG_USB_DEVICE_STACK_NEXT)
    if (storage_release_for_usb() == 0 && usb_restart(true) == 0) {
        usb_msc_active = true;
    }
#endif
}

static void usb_leave(SystemState to) {
    (void)to;
#if defined(CONFIG_USB_DEVICE_STACK_NEXT)
    if (usb_msc_active) {
        usb_msc_active = false;
        usb_restart(false);
    }
    // Remount even after a failed hand-over, in case the unmount went through
    storage_reclaim_from_usb();
#endif
}

static void usb_render(void) {
    if (usb_msc_active) {
        display_draw_message("USB Drive Mode", "SD card is shown on the computer. Eject it there, then press Select.", false);
    } else {
        display_draw_message("USB Drive Mode", "Could not share the SD card over USB. Press Select.", true);
    }
}

const Screen screen_usb_storage = {
    .enter = usb_enter,
    .leave = usb_leave,
    .render = usb_render,
    .select = back_home,
    .long_press_sleeps = true,
    .stay_awake = true,
};
