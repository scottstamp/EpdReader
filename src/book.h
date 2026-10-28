#ifndef BOOK_H
#define BOOK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "epub.h"

// The open book, the reading position in it and reading time. Main thread only.

void book_init(void);

// Name of the open book in the library ("" when none)
const char *book_name(void);
bool book_is_open(void);

// Opens a book from the library at its saved position and makes it the one to continue
void book_open(const char *name);
// Restores the book shown before sleep or reboot, without touching the progress file
void book_restore(const char *name, uint32_t offset);

// EPUB index of the open book, NULL for plain text
const EpubBook *book_epub(void);

uint32_t book_progress_percent(void);

// Index of the chapter containing the reading position (0 for plain text)
int book_chapter_index(void);
// Chapter title, or "Chapter N" when the EPUB names none; "" for plain text
void book_chapter_title(int idx, char *out, size_t out_size);

// Draws the page at the reading position under `header`; false if the file can't be read
bool book_draw_page(const char *header);

void book_turn_forward(int pages);
void book_turn_back(int pages);
void book_jump(uint32_t offset);
// Call when page breaks change (font, size, spacing)
void book_reset_pagination(void);

// Reading positions are saved a few seconds after the last page turn
void book_flush_progress(void);
int32_t book_save_due_in_ms(void); // -1 when nothing is pending
void book_service(void);           // saves once due

// Buffer free while the reader isn't drawing; lent to Bluetooth transfers on other screens
uint8_t *book_scratch(size_t *size);

// Reading time: counted while the reader is shown, gaps over 2 minutes count as a break
void reading_update(bool still_reading);
uint32_t reading_total_seconds(void);
void reading_set_total_seconds(uint32_t seconds);
uint32_t reading_session_seconds(void);

#endif // BOOK_H
