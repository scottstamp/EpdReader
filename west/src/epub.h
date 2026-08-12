#ifndef EPUB_H
#define EPUB_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define MAX_EPUB_CHAPTERS 64

typedef struct {
    char chapter_path[128];
    uint16_t compression_method; // 0 = store, 8 = deflate
    uint32_t payload_offset;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint32_t text_length;        // Processed plain text length
    uint32_t cum_offset;         // Cumulative text offset from start of book
} EpubChapterInfo;

typedef struct {
    char book_path[256];
    int chapter_count;
    int current_chapter_idx;
    uint32_t total_book_text_len;
    EpubChapterInfo chapters[MAX_EPUB_CHAPTERS];
} EpubBook;

// EPUB API
bool epub_scan_chapters(const char *epub_path, EpubBook *book);
bool epub_read_chapter_text(const char *epub_path, const EpubChapterInfo *chap_info, char *text_out, uint32_t text_max_len);
bool epub_read_book_offset(EpubBook *book, uint32_t global_offset, char *page_buf, size_t buf_size, uint32_t *bytes_read);

#endif // EPUB_H
