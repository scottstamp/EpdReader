#include "ble_upload.h"
#include "ble.h"
#include "upload_proto.h"
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <string.h>

LOG_MODULE_REGISTER(ble_upload, LOG_LEVEL_INF);

// Deepest path (fs_rename -> FatFs -> SD over SPI) measured ~1.4 KB with -fstack-usage
#define UPLOAD_STACK_SIZE  2048
#define UPLOAD_THREAD_PRIO 5

// ATT write header (3) + DATA header
#define DATA_OVERHEAD (3 + UP_DATA_HDR_LEN)

// "/SD:/books/<name>.part"; the "/SD:/cache/<name>.idx" sidecars are shorter
#define UPLOAD_PATH_MAX (sizeof(BOOK_DIR "/") + UP_NAME_MAX + sizeof(".part") - 1)

// The ring (flow-control window) is lent by the caller in ble_upload_start()
static up_session sess = {
    .max_data = 20,
};
static K_MUTEX_DEFINE(sess_lock);
static K_SEM_DEFINE(work_sem, 0, 1);
// Frames are only accepted while the upload screen is open
static bool accepting;
static struct fs_file_t part_file;

static void build_path(char *out, size_t size, const char *dir, const char *name, const char *suffix) {
    snprintf(out, size, "%s/%s%s", dir, name, suffix);
}

static void io_lock(void *ctx) {
    k_mutex_lock(&sess_lock, K_FOREVER);
}

static void io_unlock(void *ctx) {
    k_mutex_unlock(&sess_lock);
}

static int io_open(void *ctx, const char *name) {
    char path[UPLOAD_PATH_MAX];
    fs_mkdir(BOOK_DIR); // -EEXIST normally; cheaper on the stack than fs_stat
    build_path(path, sizeof(path), BOOK_DIR, name, ".part");
    fs_file_t_init(&part_file);
    int err = fs_open(&part_file, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
    if (err) {
        LOG_ERR("Cannot create %s (err %d)", path, err);
    }
    return err;
}

static int io_write(void *ctx, const uint8_t *data, size_t len) {
    ssize_t n = fs_write(&part_file, data, len);
    if (n != (ssize_t)len) {
        LOG_ERR("SD write failed (%d of %u)", (int)n, (unsigned)len);
        return n < 0 ? (int)n : -ENOSPC;
    }
    return 0;
}

static int io_close(void *ctx) {
    return fs_close(&part_file);
}

static int io_discard(void *ctx, const char *name) {
    char path[UPLOAD_PATH_MAX];
    build_path(path, sizeof(path), BOOK_DIR, name, ".part");
    return fs_unlink(path);
}

static int io_commit(void *ctx, const char *name) {
    char from[UPLOAD_PATH_MAX];
    char to[UPLOAD_PATH_MAX];
    build_path(from, sizeof(from), BOOK_DIR, name, ".part");
    build_path(to, sizeof(to), BOOK_DIR, name, "");
    // Replaces an existing book of the same name
    int err = fs_rename(from, to);
    if (err) {
        LOG_ERR("Cannot rename %s (err %d)", from, err);
        return err;
    }
    // A replaced EPUB must not open with the previous file's converted text
    static const char *const sidecars[] = { ".idx", ".txt", ".cov" };
    for (size_t i = 0; i < ARRAY_SIZE(sidecars); i++) {
        build_path(to, sizeof(to), SD_MOUNT_POINT "/cache", name, sidecars[i]);
        fs_unlink(to);
    }
    LOG_INF("Saved %s/%s", BOOK_DIR, name);
    return 0;
}

static int io_notify(void *ctx, const uint8_t *data, size_t len) {
    // Notification buffers are shared with the stack; wait briefly for one
    for (int tries = 0; tries < 50; tries++) {
        int err = ble_send_data(data, (uint16_t)len);
        if (err != -ENOMEM && err != -ENOBUFS && err != -EAGAIN) {
            return err;
        }
        k_msleep(10);
    }
    return -ENOMEM;
}

static const up_io upload_io = {
    .lock = io_lock,
    .unlock = io_unlock,
    .open = io_open,
    .write = io_write,
    .close = io_close,
    .discard = io_discard,
    .commit = io_commit,
    .notify = io_notify,
};

static void upload_thread(void *p1, void *p2, void *p3) {
    for (;;) {
        k_sem_take(&work_sem, K_FOREVER);

        uint16_t mtu = ble_get_mtu();
        k_mutex_lock(&sess_lock, K_FOREVER);
        sess.max_data = (mtu > DATA_OVERHEAD) ? (uint16_t)(mtu - DATA_OVERHEAD) : 1;
        k_mutex_unlock(&sess_lock);

        up_worker_run(&sess, &upload_io);

#if defined(CONFIG_INIT_STACKS)
        size_t unused = 0;
        if (k_thread_stack_space_get(k_current_get(), &unused) == 0) {
            LOG_INF("Upload stack unused: %u", (unsigned)unused);
        }
#endif
    }
}

K_THREAD_DEFINE(ble_upload_tid, UPLOAD_STACK_SIZE, upload_thread, NULL, NULL, NULL,
                UPLOAD_THREAD_PRIO, 0, 0);

// Bluetooth RX thread: only parses and buffers, the worker does the SD writes
static void on_data(const uint8_t *data, uint16_t len) {
    k_mutex_lock(&sess_lock, K_FOREVER);
    bool wake = accepting && up_rx_frame(&sess, data, len);
    k_mutex_unlock(&sess_lock);
    if (wake) {
        k_sem_give(&work_sem);
    }
}

static void on_status(bool connected) {
    if (connected) {
        return;
    }
    k_mutex_lock(&sess_lock, K_FOREVER);
    bool wake = up_rx_abort(&sess, false);
    k_mutex_unlock(&sess_lock);
    if (wake) {
        k_sem_give(&work_sem);
    }
}

int ble_upload_start(uint8_t *buf, size_t size) {
    if (!buf || size == 0) {
        return -EINVAL;
    }
    int err = ble_init(on_status, on_data);
    if (err) {
        return err;
    }
    k_mutex_lock(&sess_lock, K_FOREVER);
    // Idle here: ble_upload_stop() waited for it, so the old buffer is unused
    sess.ring = buf;
    sess.ring_cap = (uint32_t)size;
    accepting = true;
    k_mutex_unlock(&sess_lock);
    ble_start_advertising();
    return 0;
}

void ble_upload_stop(void) {
    k_mutex_lock(&sess_lock, K_FOREVER);
    accepting = false;
    bool wake = up_rx_abort(&sess, false);
    k_mutex_unlock(&sess_lock);
    if (wake) {
        k_sem_give(&work_sem);
    }
    ble_stop_advertising();
    ble_disconnect();

    // The worker deletes the partial file; only then is the SD card free again
    for (;;) {
        k_mutex_lock(&sess_lock, K_FOREVER);
        bool idle = up_is_idle(&sess);
        k_mutex_unlock(&sess_lock);
        if (idle) {
            break;
        }
        k_msleep(10);
    }
}

void ble_upload_get_status(BleUploadStatus *st) {
    k_mutex_lock(&sess_lock, K_FOREVER);
    st->connected = ble_is_connected();
    st->receiving = (sess.state == UP_OPENING || sess.state == UP_RECEIVING);
    snprintf(st->name, sizeof(st->name), "%s", sess.name);
    st->size = sess.size;
    st->received = sess.tail;
    st->result_seq = sess.result_seq;
    st->result_ok = (sess.result_err == UP_ERR_NONE);
    st->result_msg = up_err_str(sess.result_err);
    snprintf(st->result_name, sizeof(st->result_name), "%s", sess.result_name);
    k_mutex_unlock(&sess_lock);
}
