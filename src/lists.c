#include "lists.h"
#include "config.h"
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(lists, LOG_LEVEL_INF);

#define LISTS_FILE     SD_MOUNT_POINT "/lists.txt"
#define LISTS_TMP_FILE SD_MOUNT_POINT "/lists.tmp"

static char s_text[LISTS_DOC_MAX + 1];
static ListsDoc s_doc = { .text = s_text, .cap = LISTS_DOC_MAX };

static int save(void) {
    // Write a temp file and rename it over the old one, so a power cut can't leave half a file
    struct fs_file_t f;
    fs_file_t_init(&f);
    int err = fs_open(&f, LISTS_TMP_FILE, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
    if (err) {
        LOG_ERR("Cannot create %s (err %d)", LISTS_TMP_FILE, err);
        return err;
    }
    ssize_t n = fs_write(&f, s_doc.text, s_doc.len);
    err = fs_close(&f);
    if (n != (ssize_t)s_doc.len || err) {
        LOG_ERR("Writing %s failed", LISTS_TMP_FILE);
        fs_unlink(LISTS_TMP_FILE);
        return -EIO;
    }
    fs_unlink(LISTS_FILE);
    err = fs_rename(LISTS_TMP_FILE, LISTS_FILE);
    if (err) {
        LOG_ERR("Cannot rename %s (err %d)", LISTS_TMP_FILE, err);
    }
    return err;
}

static bool load_file(const char *path) {
    struct fs_file_t f;
    fs_file_t_init(&f);
    if (fs_open(&f, path, FS_O_READ) != 0) {
        return false;
    }
    ssize_t n = fs_read(&f, s_text, LISTS_DOC_MAX);
    fs_close(&f);
    if (n < 0) {
        return false;
    }
    s_doc.len = (uint16_t)n;
    int err = lists_doc_parse(&s_doc);
    if (err) {
        LOG_WRN("Ignoring invalid %s (error %d)", path, err);
        return false;
    }
    return true;
}

void lists_load(void) {
    // A leftover temp file means a save was cut short after it was fully written
    if (load_file(LISTS_FILE) || load_file(LISTS_TMP_FILE)) {
        LOG_INF("Lists loaded: %u lists, %u items, rev %u", s_doc.list_count, s_doc.item_count,
                (unsigned)s_doc.rev);
        return;
    }
    lists_doc_set_default(&s_doc);
}

const ListsDoc *lists_get(void) {
    return &s_doc;
}

int lists_toggle(int item_index) {
    int err = lists_doc_toggle(&s_doc, item_index);
    return err ? err : (save() ? LISTS_ERR_STORAGE : LISTS_OK);
}

int lists_clear_checked(int list_index) {
    uint32_t rev = s_doc.rev;
    int err = lists_doc_clear_checked(&s_doc, list_index);
    if (err || s_doc.rev == rev) return err;
    return save() ? LISTS_ERR_STORAGE : LISTS_OK;
}

int lists_replace(const char *text, size_t len, uint32_t base_rev) {
    if (len > LISTS_PUT_MAX) return LISTS_ERR_TOO_BIG;
    int err = lists_doc_validate(text, len, NULL);
    if (err) return err;
    if (base_rev != s_doc.rev) return LISTS_ERR_CONFLICT;

    memcpy(s_text, text, len);
    s_doc.len = (uint16_t)len;
    lists_doc_parse(&s_doc);
    lists_doc_set_rev(&s_doc, base_rev + 1);  // fits: len <= LISTS_PUT_MAX
    LOG_INF("Lists replaced by client: rev %u", (unsigned)s_doc.rev);
    return save() ? LISTS_ERR_STORAGE : LISTS_OK;
}
