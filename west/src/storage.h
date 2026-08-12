#ifndef STORAGE_H
#define STORAGE_H

#include "config.h"

int storage_init(void);
void storage_dump_tree(void);

// Book and File listing
int storage_list_sd_books(char books[][64], int max_books);
uint32_t storage_get_book_size(const char *book_name);
bool storage_read_book_page(const char *book_name, uint32_t offset, char *page_buf, size_t buf_size, uint32_t *bytes_read);

bool storage_read_progress(char *book_name, uint32_t *offset);
bool storage_write_progress(const char *book_name, uint32_t offset);

bool storage_read_stats(uint32_t *total_seconds);
bool storage_write_stats(uint32_t total_seconds);

bool storage_read_sleep_state(int *state, int *menu_idx, char *active_book);
bool storage_write_sleep_state(int state, int menu_idx, const char *active_book);
void storage_clear_sleep_state(void);

#endif // STORAGE_H
