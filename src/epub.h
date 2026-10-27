#ifndef EPUB_H
#define EPUB_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define MAX_EPUB_CHAPTERS 128

typedef struct {
    char chapter_path[128];
    char title[64];
    uint16_t compression_method; // 0 = store, 8 = deflate
    uint32_t payload_offset;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint32_t text_length;        // Processed plain text length
    uint32_t cum_offset;         // Cumulative text offset from start of book
} EpubChapterInfo;

typedef struct {
    char cover_path[128];
    uint16_t compression_method; // 0 = store, 8 = deflate
    uint32_t payload_offset;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    bool found;
} EpubCoverInfo;

typedef struct {
    char book_path[256];
    char title[128];
    char author[128];
    int chapter_count;
    int current_chapter_idx;
    uint32_t total_book_text_len;
    EpubCoverInfo cover;
    EpubChapterInfo chapters[MAX_EPUB_CHAPTERS];
} EpubBook;

// Called with 0..100 while a book is converted to its plain-text cache
typedef void (*epub_progress_cb_t)(int percent);

// EPUB API
void epub_set_progress_cb(epub_progress_cb_t cb);

// Indexes the book and, on first open, converts it to a plain-text file in
// /SD:/cache. Chapter cum_offset/text_length are byte offsets into that file.
bool epub_scan_chapters(const char *epub_path, EpubBook *book);
void epub_build_text_path(const char *epub_path, char *out, size_t max_len);
// Title, author and text length from a converted book's index header, without loading
// the chapter table. False if the book has not been converted (or the cache is stale).
bool epub_read_cached_metadata(const char *epub_path, char *title, size_t title_max,
                               char *author, size_t author_max, uint32_t *total_len);
bool epub_extract_cover_image(const EpubBook *book, uint8_t *gray_out, int target_w, int target_h);

#endif // EPUB_H
