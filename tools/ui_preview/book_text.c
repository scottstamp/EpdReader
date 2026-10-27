// Host test for src/html_text.c: converts every (X)HTML entry of an EPUB to the reader's
// plain-text format, the way the firmware builds its .txt cache (minus spine ordering), and
// writes it to a file. Run through preview.py --books, which then paginates the result.
//
//   book_text <book.epub> <out.txt>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "html_text.h"
#include "inflate_stream.h"

// inflate_stream passes one ctx to both callbacks
typedef struct {
    const uint8_t *src;
    size_t pos, len;
    HtmlText *h;
} Input;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)rd16(p) | ((uint32_t)rd16(p + 2) << 16); }

static int read_cb(void *ctx, uint8_t *buf, size_t len) {
    Input *in = ctx;
    size_t n = in->len - in->pos < len ? in->len - in->pos : len;
    memcpy(buf, in->src + in->pos, n);
    in->pos += n;
    return (int)n;
}

static int write_cb(void *ctx, const uint8_t *buf, size_t len) {
    html_text_feed(((Input *)ctx)->h, buf, len);
    return 0;
}

static int emit_cb(void *ctx, const char *buf, size_t len) {
    return fwrite(buf, 1, len, ctx) == len ? 0 : -1;
}

static int ends_with(const char *s, size_t n, const char *suffix) {
    size_t k = strlen(suffix);
    return n >= k && memcmp(s + n - k, suffix, k) == 0;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s book.epub out.txt\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *zip = malloc(size);
    if (fread(zip, 1, size, f) != (size_t)size) return 2;
    fclose(f);

    // Walk the central directory: local headers may defer sizes to a data descriptor
    long eocd = size - 22;
    while (eocd >= 0 && rd32(zip + eocd) != 0x06054b50) eocd--;
    if (eocd < 0) {
        fprintf(stderr, "%s: no zip directory\n", argv[1]);
        return 1;
    }
    uint16_t entries = rd16(zip + eocd + 10);
    const uint8_t *cd = zip + rd32(zip + eocd + 16);

    FILE *out = fopen(argv[2], "wb");
    static HtmlText h;
    html_text_init(&h, emit_cb, out);
    static uint8_t window[INFLATE_WINDOW_SIZE], inbuf[1024];
    int docs = 0, errors = 0;

    for (int i = 0; i < entries; i++, cd += 46 + rd16(cd + 28) + rd16(cd + 30) + rd16(cd + 32)) {
        const char *name = (const char *)cd + 46;
        size_t nlen = rd16(cd + 28);
        if (!ends_with(name, nlen, ".xhtml") && !ends_with(name, nlen, ".html") && !ends_with(name, nlen, ".htm")) {
            continue;
        }
        uint16_t method = rd16(cd + 10);
        uint32_t csize = rd32(cd + 20);
        const uint8_t *lh = zip + rd32(cd + 42);
        const uint8_t *data = lh + 30 + rd16(lh + 26) + rd16(lh + 28);

        if (docs++) html_text_document_break(&h);
        if (method == 0) {
            html_text_feed(&h, data, csize);
        } else if (method == 8) {
            Input in = { data, 0, csize, &h };
            int rc = inflate_stream(window, inbuf, sizeof(inbuf), read_cb, write_cb, &in);
            if (rc != 0) {
                fprintf(stderr, "%.*s: inflate error %d\n", (int)nlen, name, rc);
                errors++;
            }
        }
    }
    int rc = html_text_finish(&h);
    fclose(out);
    printf("%s: %d documents, %u bytes of text\n", argv[1], docs, html_text_total(&h));
    return (rc || errors) ? 1 : 0;
}
