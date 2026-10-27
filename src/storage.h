#ifndef STORAGE_H
#define STORAGE_H

#include "config.h"
#include "epub.h"

int storage_init(void);
// Hand the SD card to USB mass storage (unmounts FAT) and take it back (remounts)
int storage_release_for_usb(void);
int storage_reclaim_from_usb(void);
void storage_dump_tree(void);

// Book and File listing
int storage_list_sd_books(char books[][64], int max_books);
uint32_t storage_get_book_size(const char *book_name);
bool storage_get_book_metadata(const char *book_name, char *title_out, size_t title_max, char *author_out, size_t author_max);
bool storage_get_book_display_title(const char *book_name, char *out_str, size_t max_len);
// Loaded EPUB index for book_name (converted on first open), NULL for non-EPUB books
const EpubBook *storage_get_epub(const char *book_name);
bool storage_read_book_page(const char *book_name, uint32_t offset, char *page_buf, size_t buf_size, uint32_t *bytes_read);

bool storage_read_progress(char *book_name, uint32_t *offset);
bool storage_write_progress(const char *book_name, uint32_t offset);

// Legacy SD files, read once to migrate into prefs
bool storage_read_stats(uint32_t *total_seconds);

bool storage_read_sleep_state(int *state, int *menu_idx, char *active_book);
bool storage_write_sleep_state(int state, int menu_idx, const char *active_book);
void storage_clear_sleep_state(void);

bool storage_read_refresh_settings(int *profile, uint8_t *interval, uint8_t *count);

#endif // STORAGE_H
