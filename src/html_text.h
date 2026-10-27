/* html_text.h
 * Streaming XHTML -> plain text converter for EPUB chapters.
 * Bytes can be fed in arbitrary chunks; tags and entities may straddle
 * chunk boundaries. Output uses the reader's inline style codes:
 *   0x0E/0x0F italic on/off, 0x02/0x03 bold on/off, 0x15/0x16 underline on/off
 */

#ifndef HTML_TEXT_H
#define HTML_TEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns 0 on success, non-zero to signal a write failure. */
typedef int (*html_text_emit_fn)(void *ctx, const char *buf, size_t len);

#define HTML_TEXT_TAG_MAX 200
#define HTML_TEXT_STYLE_STACK 8

typedef struct {
    char name[12];
    uint8_t nest;   // same-name children opened inside this element
    uint8_t codes;  // bit0 italic, bit1 bold, bit2 underline
} HtmlStyleFrame;

typedef struct {
    html_text_emit_fn emit;
    void *ctx;
    int error;

    char obuf[512];
    size_t olen;
    uint32_t total;      // bytes emitted so far, including buffered ones

    char last;           // last visible char or '\n' emitted (0 = nothing yet)
    uint8_t nl_run;      // consecutive newlines at the end of output
    bool pending_space;

    uint8_t mode;
    char tag[HTML_TEXT_TAG_MAX + 1];
    uint16_t tag_len;
    uint8_t comment_tail; // trailing '-' count while inside <!-- -->
    char ent[12];
    uint8_t ent_len;
    char skip[8];         // element whose content is being discarded

    HtmlStyleFrame styles[HTML_TEXT_STYLE_STACK];
    uint8_t style_depth;
} HtmlText;

void html_text_init(HtmlText *h, html_text_emit_fn emit, void *ctx);
void html_text_feed(HtmlText *h, const uint8_t *buf, size_t len);
/* Separates documents (e.g. chapters) with a paragraph break and resets per-document state. */
void html_text_document_break(HtmlText *h);
/* Flushes buffered output. Returns 0 or the first emit error. */
int html_text_finish(HtmlText *h);

static inline uint32_t html_text_total(const HtmlText *h) { return h->total; }

#ifdef __cplusplus
}
#endif

#endif /* HTML_TEXT_H */
