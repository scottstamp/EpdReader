// Host tests for src/upload_proto.c (Bluetooth book upload protocol).
//
//   gcc -Wall -Wextra -Werror -O2 -I../../src -o upload_sim upload_sim.c ../../src/upload_proto.c
//   ./upload_sim                       run the unit tests
//   ./upload_sim --serve DIR [--ring N] [--worker-every N]
//       device simulator for test_upload.py: reads frames from stdin as
//       [u16 len][frame] (len 0 = run the worker), writes notifications to
//       stdout as [u16 len][event], stores files in DIR.

#include "upload_proto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

typedef struct {
    const char *dir;
    FILE *f;
    int writes;
    int fail_write_at;  // 1-based write call to fail, 0 = never
    bool stream;        // serve mode: notifications go to stdout
    uint8_t ev[256][16];
    size_t ev_len[256];
    int nev;
} Fake;

static void path_for(const Fake *fk, char *out, size_t size, const char *name, const char *suffix) {
    snprintf(out, size, "%s/%s%s", fk->dir, name, suffix);
}

static void f_lock(void *ctx) { (void)ctx; }
static void f_unlock(void *ctx) { (void)ctx; }

static int f_open(void *ctx, const char *name) {
    Fake *fk = ctx;
    char p[512];
    path_for(fk, p, sizeof(p), name, ".part");
    fk->f = fopen(p, "wb");
    return fk->f ? 0 : -5;
}

static int f_write(void *ctx, const uint8_t *data, size_t len) {
    Fake *fk = ctx;
    if (++fk->writes == fk->fail_write_at) return -5;
    return fwrite(data, 1, len, fk->f) == len ? 0 : -5;
}

static int f_close(void *ctx) {
    Fake *fk = ctx;
    int r = fclose(fk->f);
    fk->f = NULL;
    return r == 0 ? 0 : -5;
}

static int f_discard(void *ctx, const char *name) {
    char p[512];
    path_for(ctx, p, sizeof(p), name, ".part");
    return remove(p) == 0 ? 0 : -2;
}

static int f_commit(void *ctx, const char *name) {
    char from[512], to[512];
    path_for(ctx, from, sizeof(from), name, ".part");
    path_for(ctx, to, sizeof(to), name, "");
    remove(to);  // Windows rename() does not replace
    return rename(from, to) == 0 ? 0 : -5;
}

static int f_notify(void *ctx, const uint8_t *data, size_t len) {
    Fake *fk = ctx;
    if (fk->stream) {
        uint8_t hdr[2] = { (uint8_t)len, (uint8_t)(len >> 8) };
        fwrite(hdr, 1, 2, stdout);
        fwrite(data, 1, len, stdout);
        fflush(stdout);
        return 0;
    }
    if (fk->nev < 256 && len <= 16) {
        memcpy(fk->ev[fk->nev], data, len);
        fk->ev_len[fk->nev] = len;
        fk->nev++;
    }
    return 0;
}

static up_io make_io(Fake *fk) {
    up_io io = { fk, f_lock, f_unlock, f_open, f_write, f_close, f_discard, f_commit, f_notify };
    return io;
}

// ---------------------------------------------------------------- serve mode

static int read_exact(uint8_t *buf, size_t n) {
    return fread(buf, 1, n, stdin) == n;
}

static int serve(const char *dir, uint32_t ring_size, int worker_every) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    static up_session s;
    uint8_t *ring = malloc(ring_size);
    Fake fk = { .dir = dir, .stream = true };
    up_io io = make_io(&fk);
    up_init(&s, ring, ring_size);
    s.max_data = 244;

    int since = 0;
    uint8_t frame[600];
    for (;;) {
        uint8_t hdr[2];
        if (!read_exact(hdr, 2)) break;
        size_t len = hdr[0] | (hdr[1] << 8);
        if (len > sizeof(frame) || (len && !read_exact(frame, len))) break;
        bool run = (len == 0);
        if (len && up_rx_frame(&s, frame, len)) {
            run = run || (++since >= worker_every);
        }
        // START/END/errors must not wait for the batch
        if (len && (frame[0] != UP_OP_DATA || s.state == UP_FAILING)) run = true;
        if (run) {
            since = 0;
            up_worker_run(&s, &io);
        }
    }
    free(ring);
    return 0;
}

// ---------------------------------------------------------------- unit tests

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static const char *test_dir = ".";
static up_session s;
static uint8_t ring[64];
static Fake fk;
static up_io io;

static void reset(void) {
    if (fk.f) fclose(fk.f);
    memset(&fk, 0, sizeof(fk));
    fk.dir = test_dir;
    io = make_io(&fk);
    up_init(&s, ring, sizeof(ring));
    s.max_data = 10;
}

static size_t mk_start(uint8_t *b, const char *name, uint32_t size, uint32_t crc) {
    b[0] = UP_OP_START;
    b[1] = UP_VERSION;
    for (int i = 0; i < 4; i++) b[2 + i] = (uint8_t)(size >> (8 * i));
    for (int i = 0; i < 4; i++) b[6 + i] = (uint8_t)(crc >> (8 * i));
    size_t n = strlen(name);
    memcpy(b + 10, name, n);
    return 10 + n;
}

static size_t mk_data(uint8_t *b, uint32_t off, const uint8_t *p, size_t n) {
    b[0] = UP_OP_DATA;
    for (int i = 0; i < 4; i++) b[1 + i] = (uint8_t)(off >> (8 * i));
    memcpy(b + 5, p, n);
    return 5 + n;
}

static bool rx(const uint8_t *b, size_t n) {
    return up_rx_frame(&s, b, n);
}

static uint8_t last_ev(void) {
    return fk.nev ? fk.ev[fk.nev - 1][0] : 0;
}

static uint8_t last_err(void) {
    return (fk.nev && fk.ev[fk.nev - 1][0] == UP_EVT_ERROR) ? fk.ev[fk.nev - 1][1] : 0;
}

static uint32_t ev_u32(int i, int at) {
    const uint8_t *p = fk.ev[i] + at;
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool file_exists(const char *name, const char *suffix) {
    char p[512];
    path_for(&fk, p, sizeof(p), name, suffix);
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static bool file_equals(const char *name, const uint8_t *data, size_t n) {
    char p[512];
    path_for(&fk, p, sizeof(p), name, "");
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    uint8_t buf[4096];
    size_t got = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    return got == n && memcmp(buf, data, n) == 0;
}

static void rm_file(const char *name) {
    char p[512];
    path_for(&fk, p, sizeof(p), name, "");
    remove(p);
}

// Sends data honouring the window; runs the worker only when the client would block
static int send_all(const uint8_t *data, uint32_t size, uint32_t chunk) {
    uint32_t window = ev_u32(fk.nev - 1, 2);
    uint32_t sent = 0, acked = 0;
    uint8_t b[300];
    while (sent < size) {
        uint32_t n = size - sent < chunk ? size - sent : chunk;
        while (sent + n - acked > window) {
            int before = fk.nev;
            up_worker_run(&s, &io);
            for (int i = before; i < fk.nev; i++) {
                if (fk.ev[i][0] == UP_EVT_ACK) acked = ev_u32(i, 1);
                if (fk.ev[i][0] == UP_EVT_ERROR) return -1;
            }
            if (fk.nev == before) return -2;  // stalled: no ack while blocked
        }
        rx(b, mk_data(b, sent, data + sent, n));
        sent += n;
    }
    int before = fk.nev;
    up_worker_run(&s, &io);
    for (int i = before; i < fk.nev; i++) {
        if (fk.ev[i][0] == UP_EVT_ERROR) return -1;
    }
    return 0;
}

static void test_crc_and_names(void) {
    const uint8_t v[] = "123456789";
    CHECK(up_crc32(0, v, 9) == 0xCBF43926u);
    CHECK(up_crc32(up_crc32(0, v, 4), v + 4, 5) == 0xCBF43926u);
    CHECK(up_crc32(0, v, 0) == 0);

    const char *good[] = { "Book.epub", "a.TXT", "My Book (2) - Author.epub", "x.txt", "a~b_c'd.epub" };
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) CHECK(up_name_valid(good[i], strlen(good[i])));
    const char *bad[] = { "", ".epub", ".txt", "../x.epub", "a/b.epub", "a\\b.epub", "C:x.epub", "x.pdf",
                          "x..epub", ".hidden.txt", "trail.txt ", " lead.txt", "x.epub.", "a*b.txt", "a?b.txt",
                          "a|b.txt", "a\"b.txt", "a<b.txt", "tab\t.txt", "book", "epub", "Caf\xc3\xa9.epub", "del\x7f.txt" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        if (up_name_valid(bad[i], strlen(bad[i]))) printf("  accepted bad name '%s'\n", bad[i]);
        CHECK(!up_name_valid(bad[i], strlen(bad[i])));
    }
    char longname[80];
    memset(longname, 'a', sizeof(longname));
    memcpy(longname + 59, ".txt", 5);  // 63 chars
    CHECK(up_name_valid(longname, 63));
    memset(longname, 'a', sizeof(longname));
    memcpy(longname + 60, ".txt", 5);  // 64 chars
    CHECK(!up_name_valid(longname, 64));
    CHECK(!up_name_valid("a\0b.txt", 7));
}

static void test_happy_path(void) {
    reset();
    uint8_t data[1000];
    for (size_t i = 0; i < sizeof(data); i++) data[i] = (uint8_t)(i * 7 + 3);
    uint8_t b[300];
    CHECK(rx(b, mk_start(b, "ok.txt", sizeof(data), up_crc32(0, data, sizeof(data)))));
    CHECK(fk.nev == 0);  // nothing is sent from the receive side
    up_worker_run(&s, &io);
    CHECK(last_ev() == UP_EVT_READY);
    CHECK(fk.ev[0][1] == UP_VERSION);
    CHECK(ev_u32(0, 2) == sizeof(ring));
    CHECK((fk.ev[0][6] | (fk.ev[0][7] << 8)) == 10);
    CHECK(file_exists("ok.txt", ".part"));

    CHECK(send_all(data, sizeof(data), 10) == 0);
    CHECK(last_ev() == UP_EVT_ACK && ev_u32(fk.nev - 1, 1) == sizeof(data));
    CHECK(rx((const uint8_t[]){ UP_OP_END }, 1));
    up_worker_run(&s, &io);
    CHECK(last_ev() == UP_EVT_DONE);
    CHECK(ev_u32(fk.nev - 1, 1) == sizeof(data));
    CHECK(ev_u32(fk.nev - 1, 5) == up_crc32(0, data, sizeof(data)));
    CHECK(file_equals("ok.txt", data, sizeof(data)));
    CHECK(!file_exists("ok.txt", ".part"));
    CHECK(up_is_idle(&s));
    CHECK(s.result_seq == 1 && s.result_err == UP_ERR_NONE && strcmp(s.result_name, "ok.txt") == 0);

    // Second upload on the same session replaces the file
    const uint8_t small[] = "replaced";
    rx(b, mk_start(b, "ok.txt", 8, up_crc32(0, small, 8)));
    up_worker_run(&s, &io);
    CHECK(last_ev() == UP_EVT_READY);
    CHECK(send_all(small, 8, 10) == 0);
    rx((const uint8_t[]){ UP_OP_END }, 1);
    up_worker_run(&s, &io);
    CHECK(last_ev() == UP_EVT_DONE);
    CHECK(file_equals("ok.txt", small, 8));
    CHECK(s.result_seq == 2);
    rm_file("ok.txt");
}

static void test_empty_file(void) {
    reset();
    uint8_t b[64];
    rx(b, mk_start(b, "empty.txt", 0, 0));
    up_worker_run(&s, &io);
    CHECK(last_ev() == UP_EVT_READY);
    rx((const uint8_t[]){ UP_OP_END }, 1);
    up_worker_run(&s, &io);
    CHECK(last_ev() == UP_EVT_DONE);
    CHECK(file_equals("empty.txt", (const uint8_t *)"", 0));
    rm_file("empty.txt");
}

// Starts "t.txt" of size bytes and runs the worker to READY
static void start_t(uint32_t size, uint32_t crc) {
    uint8_t b[64];
    rx(b, mk_start(b, "t.txt", size, crc));
    up_worker_run(&s, &io);
    CHECK(last_ev() == UP_EVT_READY);
}

static void expect_failed(uint8_t err, bool notified) {
    up_worker_run(&s, &io);
    if (notified) {
        CHECK(last_err() == err);
    }
    CHECK(up_is_idle(&s));
    CHECK(!file_exists("t.txt", ".part"));
    CHECK(!file_exists("t.txt", ""));
}

static void test_errors(void) {
    uint8_t data[100];
    memset(data, 0x5A, sizeof(data));
    uint32_t crc = up_crc32(0, data, sizeof(data));
    uint8_t b[300];

    // Wrong CRC
    reset();
    start_t(sizeof(data), crc ^ 1);
    CHECK(send_all(data, sizeof(data), 10) == 0);
    rx((const uint8_t[]){ UP_OP_END }, 1);
    expect_failed(UP_ERR_CRC, true);
    CHECK(s.result_seq == 1 && s.result_err == UP_ERR_CRC);

    // Gap in offsets, then a fresh START recovers
    reset();
    start_t(sizeof(data), crc);
    rx(b, mk_data(b, 0, data, 10));
    rx(b, mk_data(b, 20, data, 10));
    expect_failed(UP_ERR_OFFSET, true);
    CHECK(ev_u32(fk.nev - 1, 2) == 10);
    start_t(sizeof(data), crc);
    CHECK(send_all(data, sizeof(data), 10) == 0);
    rx((const uint8_t[]){ UP_OP_END }, 1);
    up_worker_run(&s, &io);
    CHECK(last_ev() == UP_EVT_DONE);
    rm_file("t.txt");

    // Client ignores the window
    reset();
    start_t(sizeof(data), crc);
    for (uint32_t off = 0; off < sizeof(data); off += 10) rx(b, mk_data(b, off, data + off, 10));
    expect_failed(UP_ERR_OVERRUN, true);

    // Frames after an error are dropped until it is reported
    reset();
    start_t(sizeof(data), crc);
    rx(b, mk_data(b, 5, data, 10));
    CHECK(!rx(b, mk_data(b, 0, data, 10)));
    expect_failed(UP_ERR_OFFSET, true);
    CHECK(fk.nev == 2);

    // Disconnect mid-transfer: nothing to notify, partial file removed
    reset();
    start_t(sizeof(data), crc);
    rx(b, mk_data(b, 0, data, 10));
    up_worker_run(&s, &io);
    int nev = fk.nev;
    CHECK(up_rx_abort(&s, false));
    expect_failed(UP_ERR_ABORTED, false);
    CHECK(fk.nev == nev);
    CHECK(s.result_seq == 1 && s.result_err == UP_ERR_ABORTED);
    CHECK(!up_rx_abort(&s, false));  // idle: nothing to do

    // Client ABORT is acknowledged with an ERROR
    reset();
    start_t(sizeof(data), crc);
    rx((const uint8_t[]){ UP_OP_ABORT }, 1);
    expect_failed(UP_ERR_ABORTED, true);

    // Abort before the worker opened the file
    reset();
    rx(b, mk_start(b, "t.txt", sizeof(data), crc));
    up_rx_abort(&s, false);
    expect_failed(UP_ERR_ABORTED, false);
    CHECK(fk.nev == 0);

    // DATA and END without START
    reset();
    rx(b, mk_data(b, 0, data, 10));
    up_worker_run(&s, &io);
    CHECK(last_err() == UP_ERR_NOT_STARTED);
    rx((const uint8_t[]){ UP_OP_END }, 1);
    up_worker_run(&s, &io);
    CHECK(last_err() == UP_ERR_NOT_STARTED);
    CHECK(s.result_seq == 0);

    // DATA before READY was sent
    reset();
    rx(b, mk_start(b, "t.txt", sizeof(data), crc));
    rx(b, mk_data(b, 0, data, 10));
    expect_failed(UP_ERR_NOT_STARTED, true);

    // Bad names are rejected without touching the card or the UI result
    reset();
    rx(b, mk_start(b, "../evil.epub", 10, 0));
    up_worker_run(&s, &io);
    CHECK(last_err() == UP_ERR_BAD_NAME);
    CHECK(s.result_seq == 0 && up_is_idle(&s));
    rx(b, mk_start(b, "book.pdf", 10, 0));
    up_worker_run(&s, &io);
    CHECK(last_err() == UP_ERR_BAD_NAME);

    // Malformed frames
    reset();
    b[0] = UP_OP_START;
    b[1] = UP_VERSION;
    rx(b, 6);
    up_worker_run(&s, &io);
    CHECK(last_err() == UP_ERR_BAD_FRAME);
    size_t n = mk_start(b, "t.txt", 1, 0);
    b[1] = 99;
    rx(b, n);
    up_worker_run(&s, &io);
    CHECK(last_err() == UP_ERR_BAD_FRAME);
    rx((const uint8_t[]){ 0x7E }, 1);
    up_worker_run(&s, &io);
    CHECK(last_err() == UP_ERR_BAD_FRAME);
    CHECK(!rx(b, 0));

    // Too much data / END too early
    reset();
    start_t(15, crc);
    rx(b, mk_data(b, 0, data, 10));
    rx(b, mk_data(b, 10, data, 10));
    expect_failed(UP_ERR_SIZE, true);
    reset();
    start_t(sizeof(data), crc);
    rx(b, mk_data(b, 0, data, 10));
    rx((const uint8_t[]){ UP_OP_END }, 1);
    expect_failed(UP_ERR_SIZE, true);

    // START during a transfer
    reset();
    start_t(sizeof(data), crc);
    rx(b, mk_start(b, "t.txt", sizeof(data), crc));
    expect_failed(UP_ERR_BUSY, true);

    // SD write failure
    reset();
    fk.fail_write_at = 3;
    start_t(sizeof(data), crc);
    CHECK(send_all(data, sizeof(data), 10) == -1);
    CHECK(last_err() == UP_ERR_IO);
    expect_failed(UP_ERR_IO, true);
    CHECK(s.result_err == UP_ERR_IO);
}

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "--serve") == 0) {
        uint32_t ring_size = 1024;
        int every = 1;
        for (int i = 3; i + 1 < argc; i += 2) {
            if (strcmp(argv[i], "--ring") == 0) ring_size = (uint32_t)atoi(argv[i + 1]);
            if (strcmp(argv[i], "--worker-every") == 0) every = atoi(argv[i + 1]);
        }
        return serve(argv[2], ring_size, every);
    }
    if (argc >= 2) test_dir = argv[1];

    test_crc_and_names();
    test_happy_path();
    test_empty_file();
    test_errors();
    if (fk.f) fclose(fk.f);
    printf(failures ? "%d FAILURES\n" : "all upload_proto tests passed\n", failures);
    return failures ? 1 : 0;
}
