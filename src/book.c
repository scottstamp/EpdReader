#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <string.h>
#include "book.h"
#include "config.h"
#include "display.h"
#include "storage.h"

LOG_MODULE_REGISTER(book, LOG_LEVEL_INF);

// Text fetched per page; must match between drawing and measuring so page breaks agree
#define READER_PAGE_BUF_SIZE 1024
// How far before the current page to look for a paragraph start when re-paginating backwards
#define PREV_PAGE_SCAN_BYTES 3072
// How far before a page to look for the paragraph start that determines its open styles
#define STYLE_SCAN_BYTES 1024
// Reading progress is written this long after the last page turn instead of on every turn
#define PROGRESS_SAVE_DELAY_MS 3000
// Time between page turns counted as reading, at most (longer gaps are breaks)
#define READING_GAP_CAP_MS (2 * 60 * 1000)

static char active_book[64] = "";
static uint32_t current_page_offset = 0;
static uint32_t last_page_bytes_read = 300;

// Start offsets of pages turned forward from, so "previous page" is exact
static uint32_t page_history[HISTORY_SIZE];
static int page_history_len = 0;

// Shared by backwards re-pagination and style scanning
static char scan_buf[PREV_PAGE_SCAN_BYTES + 1];
// Reading one page at a time (drawing or measuring); too big for the main stack
static char page_buf[READER_PAGE_BUF_SIZE];

static bool progress_dirty = false;
static uint32_t progress_dirty_ms = 0;

static uint32_t total_reading_seconds = 0;
static uint32_t reading_mark_ms = 0;    // 0 when not in the reader
static uint32_t reading_ms_remainder = 0;
static uint32_t session_start_ms = 0;

// First open of an EPUB converts it to plain text; each progress draw is a panel refresh, so keep them sparse
static void on_book_prep_progress(int percent) {
    static int next_draw_pct;
    if (percent == 0) next_draw_pct = 0;
    if (percent >= next_draw_pct && percent < 100) {
        display_draw_progress("Preparing book (first open)...", percent);
        next_draw_pct = percent + 25;
    }
}

void book_init(void) {
    epub_set_progress_cb(on_book_prep_progress);
}

const char *book_name(void) {
    return active_book;
}

bool book_is_open(void) {
    return active_book[0] != '\0';
}

static void mark_progress_dirty(void) {
    progress_dirty = true;
    progress_dirty_ms = k_uptime_get_32();
}

void book_flush_progress(void) {
    if (progress_dirty && book_is_open()) {
        storage_write_progress(active_book, current_page_offset);
    }
    progress_dirty = false;
}

int32_t book_save_due_in_ms(void) {
    if (!progress_dirty) return -1;
    uint32_t since = k_uptime_get_32() - progress_dirty_ms;
    return (since >= PROGRESS_SAVE_DELAY_MS) ? 0 : (int32_t)(PROGRESS_SAVE_DELAY_MS - since);
}

void book_service(void) {
    if (book_save_due_in_ms() == 0) {
        book_flush_progress();
    }
}

void book_reset_pagination(void) {
    page_history_len = 0;
}

void book_open(const char *name) {
    book_flush_progress(); // the previous book's pending position
    snprintf(active_book, sizeof(active_book), "%s", name);
    current_page_offset = 0;
    uint32_t saved_offset;
    if (storage_read_book_progress(active_book, &saved_offset) &&
        saved_offset < storage_get_book_size(active_book)) {
        current_page_offset = saved_offset;
    }
    book_reset_pagination();
    // Moves the book to the top of the progress file, making it the one to continue
    storage_write_progress(active_book, current_page_offset);
    progress_dirty = false;
}

void book_restore(const char *name, uint32_t offset) {
    snprintf(active_book, sizeof(active_book), "%s", name);
    current_page_offset = offset;
    book_reset_pagination();
}

const EpubBook *book_epub(void) {
    return book_is_open() ? storage_get_epub(active_book) : NULL;
}

uint32_t book_progress_percent(void) {
    uint32_t file_size = book_is_open() ? storage_get_book_size(active_book) : 0;
    uint32_t pct = (file_size > 0) ? ((current_page_offset * 100) / file_size) : 0;
    return MIN(pct, 100);
}

int book_chapter_index(void) {
    const EpubBook *epub = book_epub();
    if (!epub) return 0;
    for (int i = 0; i < epub->chapter_count; i++) {
        uint32_t start_off = epub->chapters[i].cum_offset;
        uint32_t end_off = start_off + epub->chapters[i].text_length;
        if (current_page_offset >= start_off && current_page_offset < end_off) {
            return i;
        }
    }
    return 0;
}

void book_chapter_title(int idx, char *out, size_t out_size) {
    const EpubBook *epub = book_epub();
    if (!epub || idx < 0 || idx >= epub->chapter_count) {
        snprintf(out, out_size, "%s", "");
    } else if (epub->chapters[idx].title[0] != '\0') {
        snprintf(out, out_size, "%s", epub->chapters[idx].title);
    } else {
        snprintf(out, out_size, "Chapter %d", idx + 1);
    }
}

static void page_history_push(uint32_t offset) {
    if (page_history_len == HISTORY_SIZE) {
        memmove(page_history, page_history + 1, (HISTORY_SIZE - 1) * sizeof(page_history[0]));
        page_history_len--;
    }
    page_history[page_history_len++] = offset;
}

// Without history (after resume or a jump) the previous page boundary is unknown, so lay out
// pages forward from a nearby paragraph start until reaching the current page.
static uint32_t find_previous_page_start(uint32_t offset) {
    if (offset == 0) return 0;

    uint32_t scan_start = (offset > PREV_PAGE_SCAN_BYTES) ? offset - PREV_PAGE_SCAN_BYTES : 0;
    uint32_t got = 0;
    if (!storage_read_book_page(active_book, scan_start, scan_buf, offset - scan_start + 1, &got) || got == 0) {
        return scan_start;
    }

    uint32_t anchor = scan_start;
    if (scan_start > 0) {
        // Prefer a paragraph break, else a line break, else a space, so layout starts on a word
        char *para = strstr(scan_buf, "\n\n");
        char *line = strchr(scan_buf, '\n');
        char *space = strchr(scan_buf, ' ');
        char *cut = para ? para + 2 : (line ? line + 1 : (space ? space + 1 : scan_buf));
        anchor = scan_start + (uint32_t)(cut - scan_buf);
    }

    uint32_t page_start = anchor;
    for (int guard = 0; guard < 16 && page_start < offset; guard++) {
        if (!storage_read_book_page(active_book, page_start, page_buf, sizeof(page_buf), &got) || got == 0) {
            break;
        }
        uint32_t consumed = display_measure_reader_page(page_buf);
        if (consumed == 0 || page_start + consumed >= offset) {
            break;
        }
        page_start += consumed;
    }
    return page_start;
}

// Bytes the page starting at offset would show, without drawing it (0 at end of book)
static uint32_t measure_page_at(uint32_t offset) {
    uint32_t got = 0;
    if (!storage_read_book_page(active_book, offset, page_buf, sizeof(page_buf), &got) || got == 0) {
        return 0;
    }
    return display_measure_reader_page(page_buf);
}

// Advances up to `pages` pages (several when presses queued up during a refresh)
void book_turn_forward(int pages) {
    uint32_t book_size = storage_get_book_size(active_book);
    for (int i = 0; i < pages; i++) {
        uint32_t len = (i == 0) ? last_page_bytes_read : measure_page_at(current_page_offset);
        if (len == 0 || current_page_offset + len >= book_size) {
            break; // stay on the last page
        }
        page_history_push(current_page_offset);
        current_page_offset += len;
    }
    mark_progress_dirty();
}

void book_turn_back(int pages) {
    for (int i = 0; i < pages && current_page_offset > 0; i++) {
        if (page_history_len > 0) {
            current_page_offset = page_history[--page_history_len];
        } else {
            current_page_offset = find_previous_page_start(current_page_offset);
        }
    }
    mark_progress_dirty();
}

void book_jump(uint32_t offset) {
    current_page_offset = offset;
    book_reset_pagination();
    mark_progress_dirty();
}

// Inline styles (e.g. an italic passage) can be open where a page starts. Styles don't
// span paragraphs, so replay the style codes from the paragraph containing offset.
static uint8_t style_at_offset(uint32_t offset) {
    if (offset == 0) return 0;

    uint32_t scan_start = (offset > STYLE_SCAN_BYTES) ? offset - STYLE_SCAN_BYTES : 0;
    uint32_t got = 0;
    if (!storage_read_book_page(active_book, scan_start, scan_buf, offset - scan_start + 1, &got) || got == 0) {
        return 0;
    }

    const char *from = scan_buf;
    for (const char *para = strstr(scan_buf, "\n\n"); para; para = strstr(para + 2, "\n\n")) {
        from = para + 2;
    }

    uint8_t style = 0;
    for (const char *c = from; *c; c++) {
        switch (*c) {
            case '\x0E': style |= READER_STYLE_ITALIC; break;
            case '\x0F': style &= ~READER_STYLE_ITALIC; break;
            case '\x02': style |= READER_STYLE_BOLD; break;
            case '\x03': style &= ~READER_STYLE_BOLD; break;
            case '\x15': style |= READER_STYLE_UNDERLINE; break;
            case '\x16': style &= ~READER_STYLE_UNDERLINE; break;
            default: break;
        }
    }
    return style;
}

bool book_draw_page(const char *header) {
    uint32_t t_start = k_uptime_get_32();
    uint32_t bytes_read = 0;
    bool ok = storage_read_book_page(active_book, current_page_offset, page_buf, sizeof(page_buf), &bytes_read);
    uint32_t t_read = k_uptime_get_32();
    if (!ok || bytes_read == 0) {
        return false;
    }

    uint8_t style = style_at_offset(current_page_offset);
    uint32_t t_style = k_uptime_get_32();

    last_page_bytes_read = display_draw_reader_page(header, page_buf, book_progress_percent(), style);
    if (last_page_bytes_read == 0) last_page_bytes_read = 300;

    // The panel driver logs its own refresh breakdown just before this
    LOG_DBG("Page turn: read %u ms, style scan %u ms, layout+refresh %u ms, total %u ms",
            t_read - t_start, t_style - t_read, k_uptime_get_32() - t_style,
            k_uptime_get_32() - t_start);
    return true;
}

uint8_t *book_scratch(size_t *size) {
    *size = sizeof(scan_buf);
    return (uint8_t *)scan_buf;
}

void reading_update(bool still_reading) {
    uint32_t now = k_uptime_get_32();
    if (still_reading && session_start_ms == 0) {
        session_start_ms = MAX(now, 1);
    }
    if (reading_mark_ms != 0) {
        uint32_t elapsed = MIN(now - reading_mark_ms, READING_GAP_CAP_MS);
        reading_ms_remainder += elapsed;
        total_reading_seconds += reading_ms_remainder / 1000;
        reading_ms_remainder %= 1000;
    }
    reading_mark_ms = still_reading ? MAX(now, 1) : 0;
}

uint32_t reading_total_seconds(void) {
    return total_reading_seconds;
}

void reading_set_total_seconds(uint32_t seconds) {
    total_reading_seconds = seconds;
}

uint32_t reading_session_seconds(void) {
    return session_start_ms ? (k_uptime_get_32() - session_start_ms) / 1000 : 0;
}
