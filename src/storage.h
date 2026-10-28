#ifndef STORAGE_H
#define STORAGE_H

#include "config.h"
#include "epub.h"
#include <string.h>

int storage_init(void);
// Hand the SD card to USB mass storage (unmounts FAT) and take it back (remounts)
int storage_release_for_usb(void);
int storage_reclaim_from_usb(void);

// Library listing. Each pool entry is the filename followed by its display label,
// e.g. "Title - 42%" (no percent for books never opened).
typedef struct {
    int count;
    uint16_t name_off[MAX_BOOKS];
    char pool[BOOK_POOL_SIZE];
} BookList;

static inline const char *book_list_name(const BookList *list, int idx) {
    return &list->pool[list->name_off[idx]];
}

static inline const char *book_list_label(const BookList *list, int idx) {
    const char *name = book_list_name(list, idx);
    return name + strlen(name) + 1;
}

// Book and File listing
int storage_list_sd_books(BookList *list);
uint32_t storage_get_book_size(const char *book_name);
bool storage_get_book_metadata(const char *book_name, char *title_out, size_t title_max, char *author_out, size_t author_max);
bool storage_get_book_display_title(const char *book_name, char *out_str, size_t max_len);
// Drops the in-RAM book index, e.g. after the book file was replaced
void storage_forget_cached_book(void);
// Loaded EPUB index for book_name (converted on first open), NULL for non-EPUB books
const EpubBook *storage_get_epub(const char *book_name);
bool storage_read_book_page(const char *book_name, uint32_t offset, char *page_buf, size_t buf_size, uint32_t *bytes_read);

// Progress file: one "book:offset" line per book, most recently opened first.
// storage_read_progress returns the last opened book; writing moves the book to the top.
bool storage_read_progress(char *book_name, uint32_t *offset);
bool storage_read_book_progress(const char *book_name, uint32_t *offset);
bool storage_write_progress(const char *book_name, uint32_t offset);

// Legacy SD files, read once to migrate into prefs
bool storage_read_stats(uint32_t *total_seconds);

bool storage_read_sleep_state(int *state, int *menu_idx, char *active_book);
bool storage_write_sleep_state(int state, int menu_idx, const char *active_book);
void storage_clear_sleep_state(void);

bool storage_read_refresh_settings(int *profile, uint8_t *interval, uint8_t *count);

#endif // STORAGE_H
