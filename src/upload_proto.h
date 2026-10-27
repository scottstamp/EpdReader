#ifndef UPLOAD_PROTO_H
#define UPLOAD_PROTO_H

// Book upload protocol over the Nordic UART Service. Pure C (no Zephyr) so the
// framing, CRC and state machine can be tested on the host.
//
// Client -> device (writes to NUS RX, little-endian):
//   START  01 | ver u8 | size u32 | crc32 u32 | name (UTF-8, rest of frame)
//   DATA   02 | offset u32 | payload
//   END    03
//   ABORT  04
// Device -> client (notifications on NUS TX):
//   READY  81 | ver u8 | window u32 | max_data u16   (file opened, send data)
//   ACK    82 | written u32                          (bytes stored so far)
//   DONE   83 | size u32 | crc32 u32                 (file verified and saved)
//   ERROR  84 | code u8 | offset u32
//   RESEND 85 | offset u32                           (gap: resend from offset)
//
// Flow control: the client keeps at most `window` payload bytes sent but not
// yet acknowledged; the device buffers that much and acks as it writes to SD,
// at least every window/4 bytes and once all `size` bytes are stored. DATA is
// sent as write-without-response; START/END wait for the previous step's
// notification, so they cannot overtake data.
//
// Recovery: some hosts (Windows) occasionally drop a write-without-response.
// On a gap the device sends one RESEND and ignores data until that offset
// arrives; the client rewinds to it. Data before the expected offset is
// dropped (or trimmed if a frame straddles it), so a client may also rewind to
// its last ACK if acks stall.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UP_VERSION 2  // v2: RESEND recovery

#define UP_OP_START 0x01
#define UP_OP_DATA  0x02
#define UP_OP_END   0x03
#define UP_OP_ABORT 0x04

#define UP_EVT_READY 0x81
#define UP_EVT_ACK   0x82
#define UP_EVT_DONE  0x83
#define UP_EVT_ERROR  0x84
#define UP_EVT_RESEND 0x85

// Gaps tolerated per transfer before giving up with UP_ERR_OFFSET
#define UP_MAX_RESENDS 64

#define UP_START_HDR_LEN 10
#define UP_DATA_HDR_LEN  5

// Library entries hold 63 characters
#define UP_NAME_MAX 63

enum up_err {
    UP_ERR_NONE = 0,
    UP_ERR_BAD_FRAME,    // unknown opcode, short frame, wrong version
    UP_ERR_BAD_NAME,     // path separators, "..", not .epub/.txt
    UP_ERR_BUSY,         // START while a transfer is running
    UP_ERR_NOT_STARTED,  // DATA/END without an open transfer
    UP_ERR_OFFSET,       // DATA offset is not the next expected byte
    UP_ERR_OVERRUN,      // client exceeded the window
    UP_ERR_SIZE,         // more data than announced, or END before all of it
    UP_ERR_CRC,          // CRC32 mismatch at END
    UP_ERR_IO,           // SD card open/write/rename failed
    UP_ERR_ABORTED,      // client ABORT, disconnect or user left the screen
    UP_ERR_COUNT
};

enum up_state {
    UP_IDLE,
    UP_OPENING,     // START accepted, worker opening the file
    UP_RECEIVING,
    UP_FAILING,     // error pending, worker cleaning up
};

// Storage hooks; return 0 or a negative errno
typedef struct {
    void *ctx;
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
    int (*open)(void *ctx, const char *name);            // create/truncate the temp file for name
    int (*write)(void *ctx, const uint8_t *data, size_t len);
    int (*close)(void *ctx);
    int (*discard)(void *ctx, const char *name);         // delete the temp file
    int (*commit)(void *ctx, const char *name);          // temp file -> final name, replacing
    int (*notify)(void *ctx, const uint8_t *data, size_t len);
} up_io;

typedef struct {
    // Written by the receive side under lock
    uint8_t state;
    uint8_t rx_err;
    bool end_requested;
    bool err_notify;     // report rx_err to the client (not after a disconnect)
    char name[UP_NAME_MAX + 1];
    uint32_t size;
    uint32_t crc_expected;
    uint32_t rx_offset;
    uint32_t err_offset;
    bool awaiting_resend;   // gap seen, ignoring data until rx_offset arrives
    bool resend_pending;    // worker must send RESEND rx_offset
    uint16_t resends;

    // Byte ring of DATA payloads; head/tail are free-running counters
    uint8_t *ring;
    uint32_t ring_cap;
    uint32_t head;
    uint32_t tail;

    // Worker side
    bool file_open;
    bool file_created;   // temp file exists and must be deleted on failure
    uint32_t written;
    uint32_t crc;
    uint32_t acked;
    uint16_t max_data;   // largest DATA payload the link carries (set by the glue)

    // Outcome of the last transfer, for the UI (under lock)
    uint32_t result_seq;
    uint8_t result_err;  // UP_ERR_NONE = saved
    char result_name[UP_NAME_MAX + 1];
} up_session;

uint32_t up_crc32(uint32_t crc, const uint8_t *data, size_t len);
bool up_name_valid(const char *name, size_t len);
const char *up_err_str(uint8_t err);

void up_init(up_session *s, uint8_t *ring, uint32_t ring_cap);
// The window advertised in READY
uint32_t up_window(const up_session *s);

// Receive side: one frame written by the client. Call with the session locked.
// Returns true if the worker has something new to do.
bool up_rx_frame(up_session *s, const uint8_t *frame, size_t len);
// Disconnect or user exit: abandon any transfer. Call locked; returns true if
// the worker has to clean up.
bool up_rx_abort(up_session *s, bool tell_client);

// Worker side: does all pending file I/O and notifications, returns when idle
// until the next up_rx_* call. Takes the lock itself through io.
void up_worker_run(up_session *s, const up_io *io);
// True when no transfer is open and no work is pending. Call locked.
bool up_is_idle(const up_session *s);

#endif // UPLOAD_PROTO_H
