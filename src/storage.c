#include "storage.h"
#include "epub.h"
#include <zephyr/fs/fs.h>
#include <zephyr/fs/fs_sys.h>
#include <zephyr/storage/disk_access.h>
#include <ff.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

LOG_MODULE_REGISTER(storage, LOG_LEVEL_INF);

#define SD_DISK_NAME "SD"  // disk-name in app.overlay

static FATFS fat_fs;
static struct fs_mount_t sd_mount_point = {
    .type = FS_FATFS,
    .fs_data = &fat_fs,
    .mnt_point = SD_MOUNT_POINT,
};

static EpubBook s_cached_epub;
static bool s_epub_cached = false;
static char s_cached_epub_name[256] = {0};

static bool is_valid_book_file(const char *str) {
    size_t len = strlen(str);
    if (len < 4) return false;

    // Check 4-char extension (.txt, .epu)
    const char *ext4 = str + len - 4;
    if (tolower((unsigned char)ext4[0]) == '.') {
        if (tolower((unsigned char)ext4[1]) == 't' &&
            tolower((unsigned char)ext4[2]) == 'x' &&
            tolower((unsigned char)ext4[3]) == 't') return true;
        if (tolower((unsigned char)ext4[1]) == 'e' &&
            tolower((unsigned char)ext4[2]) == 'p' &&
            tolower((unsigned char)ext4[3]) == 'u') return true;
    }

    // Check 5-char extension (.epub)
    if (len >= 5) {
        const char *ext5 = str + len - 5;
        if (tolower((unsigned char)ext5[0]) == '.' &&
            tolower((unsigned char)ext5[1]) == 'e' &&
            tolower((unsigned char)ext5[2]) == 'p' &&
            tolower((unsigned char)ext5[3]) == 'u' &&
            tolower((unsigned char)ext5[4]) == 'b') return true;
    }
    return false;
}

static bool ends_with_epub(const char *str) {
    size_t len = strlen(str);
    if (len < 4) return false;

    const char *ext4 = str + len - 4;
    if (tolower((unsigned char)ext4[0]) == '.' &&
        tolower((unsigned char)ext4[1]) == 'e' &&
        tolower((unsigned char)ext4[2]) == 'p' &&
        tolower((unsigned char)ext4[3]) == 'u') return true;

    if (len >= 5) {
        const char *ext5 = str + len - 5;
        if (tolower((unsigned char)ext5[0]) == '.' &&
            tolower((unsigned char)ext5[1]) == 'e' &&
            tolower((unsigned char)ext5[2]) == 'p' &&
            tolower((unsigned char)ext5[3]) == 'u' &&
            tolower((unsigned char)ext5[4]) == 'b') return true;
    }
    return false;
}

static void storage_dump_dir(const char *path, int depth) {
    struct fs_dir_t dir;
    struct fs_dirent entry;

    fs_dir_t_init(&dir);
    int res = fs_opendir(&dir, path);
    if (res != 0) {
        LOG_WRN("[SD TREE] Failed to open: %s (err=%d)", path, res);
        return;
    }

    while (fs_readdir(&dir, &entry) == 0 && entry.name[0] != '\0') {
        char full_path[512];
        if (path[strlen(path) - 1] == '/') {
            snprintf(full_path, sizeof(full_path), "%.240s%.255s", path, entry.name);
        } else {
            snprintf(full_path, sizeof(full_path), "%.240s/%.255s", path, entry.name);
        }

        if (entry.type == FS_DIR_ENTRY_DIR) {
            LOG_INF("[SD TREE] [DIR]  %s", full_path);
            if (depth < 3) {
                storage_dump_dir(full_path, depth + 1);
            }
        } else {
            LOG_INF("[SD TREE] [FILE] %s (%u bytes)", full_path, (uint32_t)entry.size);
        }
    }
    fs_closedir(&dir);
}

void storage_dump_tree(void) {
    LOG_INF("===================================");
    LOG_INF("=== SD CARD DIRECTORY STRUCTURE ===");
    storage_dump_dir(SD_MOUNT_POINT, 0);
    LOG_INF("===================================");
}

int storage_init(void) {
    // Power-on stabilization delay for SD card internal controller after System OFF wake-up
    k_msleep(250);

    int res = -1;
    for (int retry = 0; retry < 5; retry++) {
        res = fs_mount(&sd_mount_point);
        if (res == 0) break;
        LOG_WRN("SD card mount attempt %d/5 failed (err=%d), retrying in 100ms...", retry + 1, res);
        k_msleep(100);
    }

    if (res != 0) {
        LOG_ERR("SD card mount failed permanently (err=%d). Running without SD card.", res);
        return res;
    }

    LOG_INF("SD card mounted successfully at %s", SD_MOUNT_POINT);
    struct fs_dirent cache_dir_stat;
    if (fs_stat(SD_MOUNT_POINT "/cache", &cache_dir_stat) != 0) {
        fs_mkdir(SD_MOUNT_POINT "/cache");
    }
    storage_dump_tree();
    return 0;
}

void storage_forget_cached_book(void) {
    s_epub_cached = false;
}

// The FAT volume must not be mounted while the host writes the card over USB
int storage_release_for_usb(void) {
    s_epub_cached = false;
    int res = fs_unmount(&sd_mount_point);
    if (res != 0) {
        LOG_ERR("SD unmount failed (err=%d)", res);
        return res;
    }
    // Unmounting powers the disk down; USB mass storage needs it running
    res = disk_access_ioctl(SD_DISK_NAME, DISK_IOCTL_CTRL_INIT, NULL);
    if (res != 0) {
        LOG_ERR("SD disk init for USB failed (err=%d)", res);
    }
    return res;
}

int storage_reclaim_from_usb(void) {
    disk_access_ioctl(SD_DISK_NAME, DISK_IOCTL_CTRL_DEINIT, NULL);
    int res = fs_mount(&sd_mount_point);
    if (res != 0) {
        LOG_ERR("SD remount failed (err=%d)", res);
    }
    return res;
}

static bool resolve_book_path(const char *book_name, char *out_path, size_t out_size) {
    if (strncmp(book_name, "/SD:", 4) == 0) {
        snprintf(out_path, out_size, "%s", book_name);
        return true;
    }

    const char *prefixes[] = {
        "/SD:/BOOKS",
        "/SD:/books",
        "/SD:/Books",
        "/SD:"
    };

    for (int i = 0; i < 4; i++) {
        snprintf(out_path, out_size, "%s/%s", prefixes[i], book_name);
        struct fs_dirent entry;
        if (fs_stat(out_path, &entry) == 0) {
            return true;
        }
    }
    snprintf(out_path, out_size, "/SD:/BOOKS/%s", book_name);
    return false;
}

// Returns the indexed EPUB for book_name, converting it on first open. Only one book
// is kept in RAM, shared by the reader, chapter list and lock screen.
static EpubBook *load_epub(const char *book_name) {
    if (!book_name || !ends_with_epub(book_name)) return NULL;

    // Called several times per page turn; skip path resolution (SD stats) on a hit
    if (s_epub_cached && strcmp(s_cached_epub_name, book_name) == 0) {
        return &s_cached_epub;
    }

    char full_path[256];
    resolve_book_path(book_name, full_path, sizeof(full_path));

    s_epub_cached = false;
    if (!epub_scan_chapters(full_path, &s_cached_epub)) {
        LOG_ERR("Failed to scan EPUB chapters: %s", full_path);
        return NULL;
    }
    snprintf(s_cached_epub_name, sizeof(s_cached_epub_name), "%s", book_name);
    s_epub_cached = true;
    return &s_cached_epub;
}

const EpubBook *storage_get_epub(const char *book_name) {
    return load_epub(book_name);
}

uint32_t storage_get_book_size(const char *book_name) {
    if (ends_with_epub(book_name)) {
        const EpubBook *book = load_epub(book_name);
        return book ? book->total_book_text_len : 0;
    }

    char full_path[256];
    resolve_book_path(book_name, full_path, sizeof(full_path));

    struct fs_dirent entry;
    if (fs_stat(full_path, &entry) == 0) {
        return (uint32_t)entry.size;
    }
    return 0;
}

// Readable title from a filename: no directory or extension, '_' and '-' as spaces
static void title_from_filename(const char *book_name, char *out, size_t max_len) {
    const char *base = strrchr(book_name, '/');
    base = base ? base + 1 : book_name;
    snprintf(out, max_len, "%s", base);

    char *dot = strrchr(out, '.');
    if (dot) *dot = '\0';

    for (int i = 0; out[i]; i++) {
        if (out[i] == '_' || out[i] == '-') {
            out[i] = ' ';
        }
    }
}

bool storage_get_book_metadata(const char *book_name, char *title_out, size_t title_max, char *author_out, size_t author_max) {
    if (!book_name || book_name[0] == '\0') return false;

    if (title_out && title_max > 0) title_out[0] = '\0';
    if (author_out && author_max > 0) author_out[0] = '\0';

    const EpubBook *book = load_epub(book_name);
    if (book) {
        if (book->title[0] != '\0' && title_out) {
            snprintf(title_out, title_max, "%s", book->title);
        }
        if (book->author[0] != '\0' && author_out) {
            snprintf(author_out, author_max, "%s", book->author);
        }
    }

    // Fallback: derive title from filename if empty
    if (title_out && title_out[0] == '\0') {
        title_from_filename(book_name, title_out, title_max);
    }
    return true;
}

bool storage_get_book_display_title(const char *book_name, char *out_str, size_t max_len) {
    if (!book_name || book_name[0] == '\0' || !out_str || max_len == 0) return false;

    char title[128] = {0};
    char author[128] = {0};
    storage_get_book_metadata(book_name, title, sizeof(title), author, sizeof(author));

    char formatted[260] = {0};
    if (author[0] != '\0') {
        snprintf(formatted, sizeof(formatted), "%.127s - %.127s", title, author);
    } else {
        snprintf(formatted, sizeof(formatted), "%.127s", title);
    }

    // Truncate to fit within top bar max length (e.g. 45 chars)
    if (strlen(formatted) > 45) {
        snprintf(out_str, max_len, "%.42s...", formatted);
    } else {
        snprintf(out_str, max_len, "%s", formatted);
    }
    return true;
}

bool storage_read_book_page(const char *book_name, uint32_t offset, char *page_buf, size_t buf_size, uint32_t *bytes_read) {
    *bytes_read = 0;
    if (!book_name || book_name[0] == '\0') return false;

    char full_path[256];

    // EPUBs are read from their converted plain-text cache
    if (ends_with_epub(book_name)) {
        const EpubBook *book = load_epub(book_name);
        if (!book) return false;
        epub_build_text_path(book->book_path, full_path, sizeof(full_path));
    } else {
        resolve_book_path(book_name, full_path, sizeof(full_path));
    }

    struct fs_file_t file;
    fs_file_t_init(&file);

    int res = -1;
    for (int retry = 0; retry < 3; retry++) {
        res = fs_open(&file, full_path, FS_O_READ);
        if (res == 0) break;
        k_msleep(50);
    }

    if (res != 0) {
        LOG_WRN("[SD Book] Failed to open %s after retries (err=%d)", full_path, res);
        return false;
    }

    if (offset > 0) {
        fs_seek(&file, offset, FS_SEEK_SET);
    }

    int bytes = fs_read(&file, page_buf, buf_size - 1);
    fs_close(&file);

    if (bytes >= 0) {
        page_buf[bytes] = '\0';
        *bytes_read = (uint32_t)bytes;
        LOG_INF("[SD Book] Read %d bytes from %s at offset %u", bytes, full_path, offset);
        return true;
    }

    return false;
}

// --- Reading progress ---
// PROGRESS_FILE holds one "book:offset" line per book, most recently opened first, so the
// first line is the book to continue. Older firmware wrote just that one line, which
// parses the same way. Book names may contain ':', so the last one separates the offset.
// Offsets are zero-padded to a fixed width so the common save (the book already on top)
// overwrites ten bytes in place instead of rewriting the file.
#define PROGRESS_TMP_FILE SD_MOUNT_POINT "/progress.tmp"
#define PROGRESS_DIGITS 10
#define PROGRESS_LINE_MAX 80  // 63-char name + ':' + digits + newline

// Return false to stop iterating
typedef bool (*progress_entry_cb)(const char *name, uint32_t offset, void *ctx);

static bool progress_open(struct fs_file_t *file) {
    fs_file_t_init(file);
    if (fs_open(file, PROGRESS_FILE, FS_O_READ) == 0) return true;
    // A rewrite interrupted after the old file was replaced leaves only the finished temp file
    fs_file_t_init(file);
    return fs_open(file, PROGRESS_TMP_FILE, FS_O_READ) == 0;
}

static bool progress_dispatch(char *line, progress_entry_cb cb, void *ctx) {
    char *cr = strchr(line, '\r');
    if (cr) *cr = '\0';

    char *colon = strrchr(line, ':');
    if (!colon || colon == line || colon[1] == '\0') return true;
    *colon = '\0';
    return cb(line, (uint32_t)strtoul(colon + 1, NULL, 10), ctx);
}

static bool progress_foreach(progress_entry_cb cb, void *ctx) {
    struct fs_file_t file;
    if (!progress_open(&file)) return false;

    char chunk[128];
    char line[PROGRESS_LINE_MAX];
    size_t len = 0;
    bool too_long = false;
    bool go_on = true;
    ssize_t rb;

    while (go_on && (rb = fs_read(&file, chunk, sizeof(chunk))) > 0) {
        for (ssize_t i = 0; i < rb && go_on; i++) {
            if (chunk[i] == '\n') {
                line[len] = '\0';
                if (!too_long) go_on = progress_dispatch(line, cb, ctx);
                len = 0;
                too_long = false;
            } else if (len < sizeof(line) - 1) {
                line[len++] = chunk[i];
            } else {
                too_long = true;
            }
        }
    }
    if (go_on && len > 0 && !too_long) {
        line[len] = '\0';
        progress_dispatch(line, cb, ctx);
    }
    fs_close(&file);
    return true;
}

typedef struct {
    const char *want;  // NULL: take the first entry
    char *name_out;
    uint32_t offset;
    bool found;
} ProgressLookup;

static bool progress_lookup_cb(const char *name, uint32_t offset, void *ctx) {
    ProgressLookup *lk = ctx;
    if (lk->want && strcmp(name, lk->want) != 0) return true;
    if (lk->name_out) snprintf(lk->name_out, 64, "%.63s", name);
    lk->offset = offset;
    lk->found = true;
    return false;
}

bool storage_read_progress(char *book_name, uint32_t *offset) {
    ProgressLookup lk = { .name_out = book_name };
    progress_foreach(progress_lookup_cb, &lk);
    if (lk.found) *offset = lk.offset;
    return lk.found;
}

bool storage_read_book_progress(const char *book_name, uint32_t *offset) {
    ProgressLookup lk = { .want = book_name };
    progress_foreach(progress_lookup_cb, &lk);
    if (lk.found) *offset = lk.offset;
    return lk.found;
}

static bool progress_write_line(struct fs_file_t *file, const char *name, uint32_t offset) {
    char line[PROGRESS_LINE_MAX];
    int len = snprintf(line, sizeof(line), "%.63s:%0*u\n", name, PROGRESS_DIGITS, offset);
    return fs_write(file, line, len) == len;
}

// Overwrites the offset digits when book_name is already the first entry
static bool progress_update_in_place(const char *book_name, uint32_t offset) {
    struct fs_file_t file;
    fs_file_t_init(&file);
    if (fs_open(&file, PROGRESS_FILE, FS_O_RDWR) != 0) return false;

    bool ok = false;
    char line[PROGRESS_LINE_MAX];
    ssize_t rb = fs_read(&file, line, sizeof(line) - 1);
    if (rb > 0) {
        line[rb] = '\0';
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *colon = strrchr(line, ':');
        size_t name_len = strlen(book_name);
        if (nl && colon && nl - colon - 1 == PROGRESS_DIGITS &&
            (size_t)(colon - line) == name_len && strncmp(line, book_name, name_len) == 0) {
            char digits[PROGRESS_DIGITS + 1];
            snprintf(digits, sizeof(digits), "%0*u", PROGRESS_DIGITS, offset);
            ok = fs_seek(&file, colon + 1 - line, FS_SEEK_SET) == 0 &&
                 fs_write(&file, digits, PROGRESS_DIGITS) == PROGRESS_DIGITS;
        }
    }
    fs_close(&file);
    return ok;
}

typedef struct {
    struct fs_file_t *out;
    const char *skip;
    int written;
    bool ok;
} ProgressCopy;

static bool progress_copy_cb(const char *name, uint32_t offset, void *ctx) {
    ProgressCopy *cp = ctx;
    if (strcmp(name, cp->skip) == 0) return true;
    if (cp->written >= MAX_PROGRESS_ENTRIES) return false;
    cp->ok = progress_write_line(cp->out, name, offset) && cp->ok;
    cp->written++;
    return true;
}

bool storage_write_progress(const char *book_name, uint32_t offset) {
    if (!book_name || book_name[0] == '\0') return false;
    if (progress_update_in_place(book_name, offset)) return true;

    // Another book (or the old format): write the reordered list to a temp file and swap it in
    struct fs_dirent st;
    if (fs_stat(PROGRESS_FILE, &st) != 0 && fs_stat(PROGRESS_TMP_FILE, &st) == 0) {
        fs_rename(PROGRESS_TMP_FILE, PROGRESS_FILE);  // finish an interrupted swap first
    }
    bool have_old = fs_stat(PROGRESS_FILE, &st) == 0;

    struct fs_file_t out;
    fs_file_t_init(&out);
    if (fs_open(&out, PROGRESS_TMP_FILE, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC) != 0) {
        return false;
    }

    ProgressCopy cp = { .out = &out, .skip = book_name, .written = 1 };
    cp.ok = progress_write_line(&out, book_name, offset);
    if (cp.ok && have_old) {
        progress_foreach(progress_copy_cb, &cp);
    }
    fs_close(&out);

    if (!cp.ok) {
        LOG_ERR("Progress write failed");
        fs_unlink(PROGRESS_TMP_FILE);
        return false;
    }
    // Zephyr's FAT rename replaces an existing destination
    return fs_rename(PROGRESS_TMP_FILE, PROGRESS_FILE) == 0;
}

// --- Library listing ---
// Titles are cut so " - 100%" still fits on a menu row
#define BOOK_TITLE_MAX 28
#define BOOK_PCT_SUFFIX_MAX 7  // " - 100%"

// Copies src shortened to max_chars bytes, ending in "..." if cut, without splitting
// a UTF-8 sequence
static void shorten_title(const char *src, char *out, size_t max_chars) {
    size_t len = strlen(src);
    if (len <= max_chars) {
        memcpy(out, src, len + 1);
        return;
    }
    size_t cut = max_chars - 3;
    while (cut > 0 && ((unsigned char)src[cut] & 0xC0) == 0x80) cut--;
    while (cut > 0 && src[cut - 1] == ' ') cut--;
    memcpy(out, src, cut);
    memcpy(out + cut, "...", 4);
}

static int book_list_find(const BookList *list, const char *name) {
    for (int i = 0; i < list->count; i++) {
        if (strcmp(book_list_name(list, i), name) == 0) return i;
    }
    return -1;
}

typedef struct {
    BookList *list;
    const uint32_t *totals;
} BookPercentCtx;

static bool book_percent_cb(const char *name, uint32_t offset, void *ctx) {
    BookPercentCtx *bp = ctx;
    int idx = book_list_find(bp->list, name);
    if (idx >= 0 && bp->totals[idx] > 0) {
        // Each label was stored with BOOK_PCT_SUFFIX_MAX spare bytes after it
        char *label = (char *)book_list_label(bp->list, idx);
        uint32_t pct = (uint32_t)MIN((uint64_t)offset * 100 / bp->totals[idx], 100);
        size_t len = strlen(label);
        snprintf(label + len, BOOK_PCT_SUFFIX_MAX + 1, " - %u%%", pct);
    }
    return true;
}

int storage_list_sd_books(BookList *list) {
    struct fs_dir_t dir;
    struct fs_dirent entry;
    uint32_t totals[MAX_BOOKS];  // text length for the percent, 0 if unknown
    size_t used = 0;
    bool full = false;

    list->count = 0;

    const char *dirs_to_check[] = {
        "/SD:/BOOKS",
        "/SD:/books",
        "/SD:/Books",
        SD_MOUNT_POINT
    };

    for (int d = 0; d < 4 && !full; d++) {
        fs_dir_t_init(&dir);
        int res = fs_opendir(&dir, dirs_to_check[d]);
        if (res != 0) {
            LOG_WRN("[SD List] Failed to open %s (err=%d)", dirs_to_check[d], res);
            continue;
        }

        while (!full) {
            if (fs_readdir(&dir, &entry) != 0 || entry.name[0] == '\0') {
                break;
            }
            // Longer names could not be reopened through the 64-byte active book name
            size_t name_len = strlen(entry.name);
            if (entry.type != FS_DIR_ENTRY_FILE || !is_valid_book_file(entry.name) || name_len > 63 ||
                book_list_find(list, entry.name) >= 0) {
                continue;
            }

            // Only the index header: loading the whole index would evict the open book's
            char title[64] = "";
            uint32_t total = 0;
            if (ends_with_epub(entry.name)) {
                epub_read_cached_metadata(entry.name, title, sizeof(title), NULL, 0, &total);
            } else {
                total = (uint32_t)entry.size;
            }
            if (title[0] == '\0') {
                title_from_filename(entry.name, title, sizeof(title));
            }
            char label[BOOK_TITLE_MAX + 1];
            shorten_title(title, label, BOOK_TITLE_MAX);
            size_t label_len = strlen(label);

            size_t need = name_len + 1 + label_len + 1 + BOOK_PCT_SUFFIX_MAX;
            if (list->count >= MAX_BOOKS || used + need > sizeof(list->pool)) {
                LOG_WRN("[SD List] Library full at %d books", list->count);
                full = true;
                break;
            }

            list->name_off[list->count] = (uint16_t)used;
            memcpy(&list->pool[used], entry.name, name_len + 1);
            memcpy(&list->pool[used + name_len + 1], label, label_len + 1);
            totals[list->count] = total;
            list->count++;
            used += need;
            LOG_INF("[SD Book Found] %s (in %s)", entry.name, dirs_to_check[d]);
        }
        fs_closedir(&dir);
    }

    BookPercentCtx bp = { .list = list, .totals = totals };
    progress_foreach(book_percent_cb, &bp);
    return list->count;
}

bool storage_read_stats(uint32_t *out_total_seconds) {
    *out_total_seconds = 0;
    struct fs_dirent entry;
    if (fs_stat(STATS_FILE, &entry) != 0) {
        return false;
    }

    struct fs_file_t file;
    fs_file_t_init(&file);
    if (fs_open(&file, STATS_FILE, FS_O_READ) != 0) {
        return false;
    }

    char line[32] = {0};
    int bytes = fs_read(&file, line, sizeof(line) - 1);
    fs_close(&file);

    if (bytes <= 0) return false;
    line[bytes] = '\0';

    *out_total_seconds = (uint32_t)strtoul(line, NULL, 10);
    return true;
}


#define SLEEP_STATE_FILE SD_MOUNT_POINT "/sleep.txt"

bool storage_read_sleep_state(int *state, int *menu_idx, char *active_book) {
    struct fs_dirent entry;
    if (fs_stat(SLEEP_STATE_FILE, &entry) != 0) return false;

    struct fs_file_t file;
    fs_file_t_init(&file);
    if (fs_open(&file, SLEEP_STATE_FILE, FS_O_READ) != 0) return false;

    char line[256] = {0};
    int bytes = fs_read(&file, line, sizeof(line) - 1);
    fs_close(&file);
    if (bytes <= 0) return false;
    line[bytes] = '\0';

    // Format: "%d:%d:%s\n"
    char *c1 = strchr(line, ':');
    if (!c1) return false;
    *c1 = '\0';
    *state = atoi(line);

    char *c2 = strchr(c1 + 1, ':');
    if (!c2) return false;
    *c2 = '\0';
    *menu_idx = atoi(c1 + 1);

    char *book_ptr = c2 + 1;
    char *nl = strpbrk(book_ptr, "\r\n");
    if (nl) *nl = '\0';

    snprintf(active_book, 64, "%s", book_ptr);
    return true;
}

bool storage_write_sleep_state(int state, int menu_idx, const char *active_book) {
    struct fs_file_t file;
    fs_file_t_init(&file);
    if (fs_open(&file, SLEEP_STATE_FILE, FS_O_CREATE | FS_O_WRITE) != 0) return false;

    char line[256];
    int len = snprintf(line, sizeof(line), "%d:%d:%s\n", state, menu_idx, active_book ? active_book : "");
    fs_write(&file, line, len);
    fs_close(&file);
    return true;
}

void storage_clear_sleep_state(void) {
    fs_unlink(SLEEP_STATE_FILE);
}

#define REFRESH_SETTINGS_FILE SD_MOUNT_POINT "/refresh.txt"

bool storage_read_refresh_settings(int *profile, uint8_t *interval, uint8_t *count) {
    struct fs_dirent entry;
    if (fs_stat(REFRESH_SETTINGS_FILE, &entry) != 0) return false;

    struct fs_file_t file;
    fs_file_t_init(&file);
    if (fs_open(&file, REFRESH_SETTINGS_FILE, FS_O_READ) != 0) return false;

    char line[64] = {0};
    int bytes = fs_read(&file, line, sizeof(line) - 1);
    fs_close(&file);
    if (bytes <= 0) return false;
    line[bytes] = '\0';

    // Format: "%d:%u:%u\n"
    char *c1 = strchr(line, ':');
    if (!c1) return false;
    *c1 = '\0';
    *profile = atoi(line);

    char *c2 = strchr(c1 + 1, ':');
    if (!c2) return false;
    *c2 = '\0';
    *interval = (uint8_t)atoi(c1 + 1);
    *count = (uint8_t)atoi(c2 + 1);

    return true;
}


