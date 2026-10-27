#include "upload_proto.h"
#include <string.h>

// CRC-32/ISO-HDLC (zlib, Python's zlib.crc32), nibble table to keep flash small
uint32_t up_crc32(uint32_t crc, const uint8_t *data, size_t len) {
    static const uint32_t tbl[16] = {
        0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
        0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
    };
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        crc = (crc >> 4) ^ tbl[crc & 0x0F];
        crc = (crc >> 4) ^ tbl[crc & 0x0F];
    }
    return ~crc;
}

static bool ends_with_ci(const char *s, size_t len, const char *suffix) {
    size_t n = strlen(suffix);
    if (len <= n) return false;  // needs a non-empty stem
    for (size_t i = 0; i < n; i++) {
        char c = s[len - n + i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != suffix[i]) return false;
    }
    return true;
}

// A bare file name the FAT volume can store as-is in the books folder
bool up_name_valid(const char *name, size_t len) {
    if (len == 0 || len > UP_NAME_MAX) return false;
    // FAT drops leading/trailing dots and spaces, which would change the name
    if (name[0] == '.' || name[0] == ' ' || name[len - 1] == '.' || name[len - 1] == ' ') return false;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = (uint8_t)name[i];
        // ASCII only: FatFs runs with code page 437 here, so UTF-8 names would
        // show up garbled on a computer
        if (c < 0x20 || c >= 0x7F || strchr("/\\:*?\"<>|", c)) return false;
        if (c == '.' && i + 1 < len && name[i + 1] == '.') return false;
    }
    return ends_with_ci(name, len, ".epub") || ends_with_ci(name, len, ".txt");
}

const char *up_err_str(uint8_t err) {
    static const char *const names[UP_ERR_COUNT] = {
        [UP_ERR_NONE] = "OK",
        [UP_ERR_BAD_FRAME] = "Protocol error",
        [UP_ERR_BAD_NAME] = "Invalid file name",
        [UP_ERR_BUSY] = "Busy",
        [UP_ERR_NOT_STARTED] = "No transfer open",
        [UP_ERR_OFFSET] = "Data out of order",
        [UP_ERR_OVERRUN] = "Buffer overrun",
        [UP_ERR_SIZE] = "Size mismatch",
        [UP_ERR_CRC] = "Checksum mismatch",
        [UP_ERR_IO] = "SD card write failed",
        [UP_ERR_ABORTED] = "Transfer cancelled",
    };
    return (err < UP_ERR_COUNT) ? names[err] : "Unknown error";
}

static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void up_init(up_session *s, uint8_t *ring, uint32_t ring_cap) {
    memset(s, 0, sizeof(*s));
    s->ring = ring;
    s->ring_cap = ring_cap;
    s->max_data = 20;  // default ATT MTU 23 minus the write header
}

uint32_t up_window(const up_session *s) {
    return s->ring_cap;
}

bool up_is_idle(const up_session *s) {
    return s->state == UP_IDLE;
}

// First error wins; the worker reports it and drops the transfer
static bool rx_fail(up_session *s, uint8_t err, uint32_t offset, bool notify) {
    if (s->state == UP_FAILING) return false;
    s->state = UP_FAILING;
    s->rx_err = err;
    s->err_offset = offset;
    s->err_notify = notify;
    return true;
}

static bool rx_start(up_session *s, const uint8_t *f, size_t len) {
    if (s->state != UP_IDLE) return rx_fail(s, UP_ERR_BUSY, 0, true);
    if (len < UP_START_HDR_LEN || f[1] != UP_VERSION) return rx_fail(s, UP_ERR_BAD_FRAME, 0, true);
    const char *name = (const char *)f + UP_START_HDR_LEN;
    size_t name_len = len - UP_START_HDR_LEN;
    if (!up_name_valid(name, name_len)) return rx_fail(s, UP_ERR_BAD_NAME, 0, true);

    memcpy(s->name, name, name_len);
    s->name[name_len] = '\0';
    s->size = get_u32(f + 2);
    s->crc_expected = get_u32(f + 6);
    s->rx_offset = 0;
    s->head = 0;
    s->tail = 0;
    s->end_requested = false;
    s->state = UP_OPENING;
    return true;
}

static bool rx_data(up_session *s, const uint8_t *f, size_t len) {
    if (s->state != UP_RECEIVING) return rx_fail(s, UP_ERR_NOT_STARTED, 0, true);
    if (len <= UP_DATA_HDR_LEN) return rx_fail(s, UP_ERR_BAD_FRAME, s->rx_offset, true);
    uint32_t offset = get_u32(f + 1);
    uint32_t n = (uint32_t)(len - UP_DATA_HDR_LEN);
    if (offset != s->rx_offset) return rx_fail(s, UP_ERR_OFFSET, s->rx_offset, true);
    if (s->end_requested || n > s->size - s->rx_offset) return rx_fail(s, UP_ERR_SIZE, s->rx_offset, true);
    if (n > s->ring_cap - (s->head - s->tail)) return rx_fail(s, UP_ERR_OVERRUN, s->rx_offset, true);

    const uint8_t *src = f + UP_DATA_HDR_LEN;
    uint32_t idx = s->head % s->ring_cap;
    uint32_t first = s->ring_cap - idx;
    if (first > n) first = n;
    memcpy(s->ring + idx, src, first);
    memcpy(s->ring, src + first, n - first);
    s->head += n;
    s->rx_offset += n;
    return true;
}

bool up_rx_frame(up_session *s, const uint8_t *frame, size_t len) {
    if (len == 0) return false;
    // Frames still in flight after an error are dropped until it is reported
    if (s->state == UP_FAILING) return false;

    switch (frame[0]) {
        case UP_OP_START:
            return rx_start(s, frame, len);
        case UP_OP_DATA:
            return rx_data(s, frame, len);
        case UP_OP_END:
            if (s->state != UP_RECEIVING) return rx_fail(s, UP_ERR_NOT_STARTED, 0, true);
            if (len != 1) return rx_fail(s, UP_ERR_BAD_FRAME, s->rx_offset, true);
            if (s->end_requested || s->rx_offset != s->size) return rx_fail(s, UP_ERR_SIZE, s->rx_offset, true);
            s->end_requested = true;
            return true;
        case UP_OP_ABORT:
            return up_rx_abort(s, true);
        default:
            return rx_fail(s, UP_ERR_BAD_FRAME, s->rx_offset, true);
    }
}

bool up_rx_abort(up_session *s, bool tell_client) {
    if (s->state == UP_IDLE) return false;
    return rx_fail(s, UP_ERR_ABORTED, s->rx_offset, tell_client);
}

static void send_ready(up_session *s, const up_io *io) {
    uint8_t ev[8];
    ev[0] = UP_EVT_READY;
    ev[1] = UP_VERSION;
    uint32_t window = up_window(s);
    // Keep at least four frames per window so acks arrive before the client stalls
    uint16_t max_data = s->max_data;
    if (max_data > window / 4) max_data = (uint16_t)(window / 4);
    put_u32(ev + 2, window);
    put_u16(ev + 6, max_data);
    io->notify(io->ctx, ev, sizeof(ev));
}

static void send_ack(up_session *s, const up_io *io) {
    uint8_t ev[5];
    ev[0] = UP_EVT_ACK;
    put_u32(ev + 1, s->written);
    io->notify(io->ctx, ev, sizeof(ev));
    s->acked = s->written;
}

// Worker-side failure: queue it like a receive error so one path cleans up
static void worker_fail(up_session *s, const up_io *io, uint8_t err) {
    io->lock(io->ctx);
    rx_fail(s, err, s->written, true);
    io->unlock(io->ctx);
}

static void handle_failure(up_session *s, const up_io *io) {
    if (s->file_open) {
        io->close(io->ctx);
        s->file_open = false;
    }
    if (s->file_created) {
        io->discard(io->ctx, s->name);
        s->file_created = false;
    }

    io->lock(io->ctx);
    uint8_t err = s->rx_err;
    uint32_t offset = s->err_offset;
    bool notify = s->err_notify;
    if (s->name[0] != '\0') {
        s->result_seq++;
        s->result_err = err;
        memcpy(s->result_name, s->name, sizeof(s->result_name));
    }
    s->name[0] = '\0';
    s->rx_err = UP_ERR_NONE;
    s->end_requested = false;
    s->head = s->tail = 0;
    s->state = UP_IDLE;
    io->unlock(io->ctx);

    if (notify) {
        uint8_t ev[6];
        ev[0] = UP_EVT_ERROR;
        ev[1] = err;
        put_u32(ev + 2, offset);
        io->notify(io->ctx, ev, sizeof(ev));
    }
}

static void finish(up_session *s, const up_io *io) {
    s->file_open = false;
    if (io->close(io->ctx) != 0) {
        worker_fail(s, io, UP_ERR_IO);
        return;
    }
    if (s->written != s->size) {
        worker_fail(s, io, UP_ERR_SIZE);
        return;
    }
    if (s->crc != s->crc_expected) {
        worker_fail(s, io, UP_ERR_CRC);
        return;
    }
    if (io->commit(io->ctx, s->name) != 0) {
        worker_fail(s, io, UP_ERR_IO);
        return;
    }
    s->file_created = false;

    io->lock(io->ctx);
    s->result_seq++;
    s->result_err = UP_ERR_NONE;
    memcpy(s->result_name, s->name, sizeof(s->result_name));
    s->name[0] = '\0';
    s->end_requested = false;
    s->state = UP_IDLE;
    io->unlock(io->ctx);

    uint8_t ev[9];
    ev[0] = UP_EVT_DONE;
    put_u32(ev + 1, s->written);
    put_u32(ev + 5, s->crc);
    io->notify(io->ctx, ev, sizeof(ev));
}

void up_worker_run(up_session *s, const up_io *io) {
    // Ack often enough that a client blocked on the window always gets one
    uint32_t ack_step = up_window(s) / 4;

    for (;;) {
        io->lock(io->ctx);
        uint8_t state = s->state;
        uint32_t tail = s->tail;
        uint32_t avail = s->head - s->tail;
        bool end = s->end_requested;
        io->unlock(io->ctx);

        if (state == UP_FAILING) {
            handle_failure(s, io);
            continue;
        }

        if (state == UP_OPENING) {
            // name is stable until the state returns to idle
            if (!s->file_open) {
                if (io->open(io->ctx, s->name) != 0) {
                    worker_fail(s, io, UP_ERR_IO);
                    continue;
                }
                s->file_open = true;
                s->file_created = true;
                s->written = 0;
                s->crc = 0;
                s->acked = 0;
            }
            bool ready = false;
            io->lock(io->ctx);
            if (s->state == UP_OPENING) {
                s->state = UP_RECEIVING;
                ready = true;
            }
            io->unlock(io->ctx);
            if (ready) send_ready(s, io);
            continue;
        }

        if (state != UP_RECEIVING) return;

        if (avail > 0) {
            uint32_t idx = tail % s->ring_cap;
            uint32_t n = s->ring_cap - idx;
            if (n > avail) n = avail;
            if (io->write(io->ctx, s->ring + idx, n) != 0) {
                worker_fail(s, io, UP_ERR_IO);
                continue;
            }
            s->crc = up_crc32(s->crc, s->ring + idx, n);
            s->written += n;
            io->lock(io->ctx);
            s->tail += n;
            io->unlock(io->ctx);
            // The final ack tells the client everything is stored before it sends END
            if (s->written - s->acked >= ack_step || s->written == s->size) send_ack(s, io);
            continue;
        }

        if (end) {
            finish(s, io);
            continue;
        }
        return;
    }
}
