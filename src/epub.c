#include "epub.h"
#include "inflate_stream.h"
#include "html_text.h"
#include "dither.h"
#include "tjpgd.h"
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

LOG_MODULE_REGISTER(epub, LOG_LEVEL_INF);

// A 4:2:0 baseline JPEG needs ~4 KB of TJpgDec pool with JD_SZBUF 1024
#define COVER_JPEG_POOL 6144
// An MCU row is at most 16 px tall and output never upscales, so it touches at most 17 rows
#define COVER_ACC_ROWS  32
#define COVER_ACC_MAX_W 160

// Book conversion and cover extraction never run at the same time, so they share one work area
static union {
    struct {
        uint8_t window[INFLATE_WINDOW_SIZE];
        uint8_t in[1024];
    } text;
    struct {
        uint32_t pool[COVER_JPEG_POOL / 4];                // TJpgDec work area (word aligned)
        uint16_t acc[COVER_ACC_ROWS][COVER_ACC_MAX_W];     // ring of output-row pixel sums
        uint8_t row[COVER_ACC_MAX_W];                      // one full output row, margins included
        uint32_t hist[256];                                // pass 1: tone histogram
        uint8_t lut[256];
        int16_t err[2 * COVER_ACC_MAX_W];                  // pass 2: dithering error rows
    } cover;
} s_work;

static HtmlText s_html;
static epub_progress_cb_t s_progress_cb;

void epub_set_progress_cb(epub_progress_cb_t cb) {
    s_progress_cb = cb;
}

static int my_strncasecmp(const char *s1, const char *s2, size_t n) {
    while (n && *s1 && *s2) {
        int diff = tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
        if (diff != 0) return diff;
        s1++;
        s2++;
        n--;
    }
    if (n == 0) return 0;
    return tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
}

static int my_strcasecmp(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        int diff = tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
        if (diff != 0) return diff;
        s1++;
        s2++;
    }
    return tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
}

static const char *my_strcasestr(const char *haystack, const char *needle) {
    if (!*needle) return haystack;
    size_t needle_len = strlen(needle);
    for (; *haystack; haystack++) {
        if (my_strncasecmp(haystack, needle, needle_len) == 0) {
            return haystack;
        }
    }
    return NULL;
}

static int natural_chapter_cmp(const void *a, const void *b) {
    const char *s1 = ((const EpubChapterInfo *)a)->chapter_path;
    const char *s2 = ((const EpubChapterInfo *)b)->chapter_path;

    while (*s1 && *s2) {
        if (isdigit((unsigned char)*s1) && isdigit((unsigned char)*s2)) {
            unsigned long n1 = strtoul(s1, (char **)&s1, 10);
            unsigned long n2 = strtoul(s2, (char **)&s2, 10);
            if (n1 != n2) {
                return (n1 < n2) ? -1 : 1;
            }
        } else {
            int diff = tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
            if (diff != 0) return diff;
            s1++;
            s2++;
        }
    }
    return (*s1 == '\0') ? ((*s2 == '\0') ? 0 : -1) : 1;
}
static void parse_toc_ncx_titles(const char *ncx_buf, EpubBook *book)
{
    if (!ncx_buf || !book || book->chapter_count == 0) return;

    const char *p = ncx_buf;
    while (*p) {
        const char *np = my_strcasestr(p, "<navPoint");
        if (!np) break;

        const char *np_end_tag = strchr(np, '>');
        if (!np_end_tag) break;
        p = np_end_tag + 1;

        const char *next_np = my_strcasestr(p, "<navPoint");
        const char *close_np = my_strcasestr(p, "</navPoint>");

        const char *block_limit = close_np;
        if (next_np && (!block_limit || next_np < block_limit)) {
            block_limit = next_np;
        }
        if (!block_limit) block_limit = p + strlen(p);

        const char *lbl = my_strcasestr(p, "<text>");
        if (!lbl || lbl >= block_limit) continue;

        lbl += 6;
        const char *lbl_end = strchr(lbl, '<');
        if (!lbl_end || lbl_end >= block_limit) continue;

        char title[64] = {0};
        size_t tlen = (size_t)(lbl_end - lbl);
        if (tlen >= sizeof(title)) tlen = sizeof(title) - 1;
        memcpy(title, lbl, tlen);
        title[tlen] = '\0';

        const char *src = my_strcasestr(p, "src=");
        if (!src || src >= block_limit) continue;

        src += 4;
        if (*src == '"' || *src == '\'') src++;
        const char *src_end = strpbrk(src, "\"'>\t\r\n ");
        if (src_end && src_end > src && src_end <= block_limit) {
            char target_href[128] = {0};
            size_t hlen = (size_t)(src_end - src);
            if (hlen >= sizeof(target_href)) hlen = sizeof(target_href) - 1;
            memcpy(target_href, src, hlen);
            target_href[hlen] = '\0';

            /* Strip #anchor fragment */
            char *hash = strchr(target_href, '#');
            if (hash) *hash = '\0';

            char *base = strrchr(target_href, '/');
            if (!base) base = strrchr(target_href, '\\');
            base = base ? (base + 1) : target_href;

            for (int c = 0; c < book->chapter_count; c++) {
                char *cbase = strrchr(book->chapters[c].chapter_path, '/');
                if (!cbase) cbase = strrchr(book->chapters[c].chapter_path, '\\');
                cbase = cbase ? (cbase + 1) : book->chapters[c].chapter_path;

                if (my_strcasecmp(cbase, base) == 0) {
                    snprintf(book->chapters[c].title, sizeof(book->chapters[c].title), "%s", title);
                    break;
                }
            }
        }
    }
}

// Copies the value of attribute `attr` from the tag spanning [tag, tag_end)
static bool tag_attr(const char *tag, const char *tag_end, const char *attr, char *out, size_t out_size)
{
    size_t alen = strlen(attr);
    for (const char *p = tag; p + alen + 2 < tag_end; p++) {
        // Attribute names follow whitespace, which rules out e.g. "idref" matching "id"
        if (!isspace((unsigned char)p[0])) continue;
        if (my_strncasecmp(p + 1, attr, alen) != 0 || p[1 + alen] != '=') continue;

        const char *v = p + 2 + alen;
        char quote = *v;
        if (quote == '"' || quote == '\'') {
            v++;
        } else {
            quote = 0;
        }
        const char *e = v;
        while (e < tag_end && (quote ? *e != quote : (!isspace((unsigned char)*e) && *e != '/'))) e++;

        size_t n = (size_t)(e - v);
        if (n >= out_size) n = out_size - 1;
        memcpy(out, v, n);
        out[n] = '\0';
        return true;
    }
    return false;
}

static bool manifest_href_for_id(const char *m_start, const char *m_end, const char *id,
                                 char *href_out, size_t href_max)
{
    char item_id[64];
    const char *p = m_start;
    while (p < m_end) {
        const char *item = my_strcasestr(p, "<item");
        if (!item || item >= m_end) break;
        const char *tag_end = strchr(item, '>');
        if (!tag_end || tag_end > m_end) break;

        if (isspace((unsigned char)item[5]) &&
            tag_attr(item, tag_end, "id", item_id, sizeof(item_id)) && strcmp(item_id, id) == 0) {
            return tag_attr(item, tag_end, "href", href_out, href_max);
        }
        p = tag_end + 1;
    }
    return false;
}

static const char *path_basename(const char *path)
{
    const char *base = strrchr(path, '/');
    if (!base) base = strrchr(path, '\\');
    return base ? (base + 1) : path;
}

// Reorders book->chapters to follow the OPF <spine>, dropping entries not in the spine.
// Works in place so it needs no heap (the old version needed ~52 KB from a 32 KB heap).
static bool parse_opf_spine_order(const char *opf_buf, EpubBook *book)
{
    if (!opf_buf || !book || book->chapter_count == 0) return false;

    const char *m_start = my_strcasestr(opf_buf, "<manifest");
    const char *m_end = m_start ? my_strcasestr(m_start, "</manifest>") : NULL;
    const char *s_start = my_strcasestr(opf_buf, "<spine");
    const char *s_end = s_start ? my_strcasestr(s_start, "</spine>") : NULL;
    if (!m_start || !m_end || !s_start || !s_end) return false;

    int16_t order[MAX_EPUB_CHAPTERS];
    bool used[MAX_EPUB_CHAPTERS] = {0};
    int ordered_count = 0;

    const char *p = s_start;
    while (p < s_end && ordered_count < book->chapter_count) {
        const char *ref = my_strcasestr(p, "<itemref");
        if (!ref || ref >= s_end) break;
        const char *ref_end = strchr(ref, '>');
        if (!ref_end) break;
        p = ref_end + 1;

        char idref[64];
        char href[128];
        if (!tag_attr(ref, ref_end, "idref", idref, sizeof(idref)) ||
            !manifest_href_for_id(m_start, m_end, idref, href, sizeof(href))) {
            continue;
        }

        char *hash = strchr(href, '#');
        if (hash) *hash = '\0';
        const char *mbase = path_basename(href);

        for (int c = 0; c < book->chapter_count; c++) {
            if (!used[c] && my_strcasecmp(path_basename(book->chapters[c].chapter_path), mbase) == 0) {
                used[c] = true;
                order[ordered_count++] = (int16_t)c;
                break;
            }
        }
    }

    if (ordered_count == 0) return false;

    // Apply the permutation with swaps, tracking where each original chapter currently sits
    int16_t slot_of[MAX_EPUB_CHAPTERS];
    int16_t at_slot[MAX_EPUB_CHAPTERS];
    for (int i = 0; i < book->chapter_count; i++) {
        slot_of[i] = (int16_t)i;
        at_slot[i] = (int16_t)i;
    }
    for (int i = 0; i < ordered_count; i++) {
        int16_t src = slot_of[order[i]];
        if (src == i) continue;

        EpubChapterInfo tmp = book->chapters[i];
        book->chapters[i] = book->chapters[src];
        book->chapters[src] = tmp;

        int16_t displaced = at_slot[i];
        at_slot[src] = displaced;
        slot_of[displaced] = src;
        at_slot[i] = order[i];
        slot_of[order[i]] = (int16_t)i;
    }

    book->chapter_count = ordered_count;
    return true;
}

#define EPUB_CACHE_MAGIC   0x45505542u  /* "EPUB" */
#define EPUB_CACHE_VERSION 9u  /* v9: typographic punctuation kept as UTF-8 */

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t total_book_text_len;
    uint16_t chapter_count;
    uint16_t reserved;
    EpubCoverInfo cover;
    char title[64];
    char author[64];
} EpubCacheHeader;

// Sidecar files live in /SD:/cache named after the book, e.g. "Book.epub.idx"
static void build_sidecar_path(const char *epub_path, const char *ext, char *out, size_t max_len)
{
    const char *base = path_basename(epub_path);
    int max_base = (max_len > 16) ? (int)(max_len - 16) : 0;
    snprintf(out, max_len, "/SD:/cache/%.*s.%s", max_base, base, ext);
}

void epub_build_text_path(const char *epub_path, char *out, size_t max_len)
{
    build_sidecar_path(epub_path, "txt", out, max_len);
}

static bool load_sidecar_cache(const char *cache_path, EpubBook *book)
{
    struct fs_file_t f;
    fs_file_t_init(&f);
    if (fs_open(&f, cache_path, FS_O_READ) != 0) return false;

    EpubCacheHeader hdr;
    ssize_t rb = fs_read(&f, &hdr, sizeof(hdr));
    if (rb != sizeof(hdr) || hdr.magic != EPUB_CACHE_MAGIC || hdr.version != EPUB_CACHE_VERSION) {
        fs_close(&f);
        return false;
    }

    if (hdr.chapter_count > MAX_EPUB_CHAPTERS) {
        fs_close(&f);
        return false;
    }

    book->chapter_count = (int)hdr.chapter_count;
    book->total_book_text_len = hdr.total_book_text_len;
    book->cover = hdr.cover;
    strncpy(book->title, hdr.title, sizeof(book->title) - 1);
    strncpy(book->author, hdr.author, sizeof(book->author) - 1);

    size_t chap_bytes = sizeof(EpubChapterInfo) * hdr.chapter_count;
    rb = fs_read(&f, book->chapters, chap_bytes);
    fs_close(&f);

    if ((size_t)rb != chap_bytes) return false;

    LOG_INF("Instant load from cache: %s (%d chapters, cover_found: %d)",
            cache_path, book->chapter_count, book->cover.found);
    return true;
}

bool epub_read_cached_metadata(const char *epub_path, char *title, size_t title_max,
                               char *author, size_t author_max, uint32_t *total_len)
{
    char cache_path[256];
    build_sidecar_path(epub_path, "idx", cache_path, sizeof(cache_path));

    struct fs_file_t f;
    fs_file_t_init(&f);
    if (fs_open(&f, cache_path, FS_O_READ) != 0) return false;

    EpubCacheHeader hdr;
    ssize_t rb = fs_read(&f, &hdr, sizeof(hdr));
    fs_close(&f);
    if (rb != sizeof(hdr) || hdr.magic != EPUB_CACHE_MAGIC || hdr.version != EPUB_CACHE_VERSION) {
        return false;
    }

    if (title && title_max > 0) snprintf(title, title_max, "%.*s", (int)sizeof(hdr.title), hdr.title);
    if (author && author_max > 0) snprintf(author, author_max, "%.*s", (int)sizeof(hdr.author), hdr.author);
    if (total_len) *total_len = hdr.total_book_text_len;
    return true;
}

static void save_sidecar_cache(const char *cache_path, const EpubBook *book)
{
    struct fs_dirent cache_dir_stat;
    if (fs_stat("/SD:/cache", &cache_dir_stat) != 0) {
        fs_mkdir("/SD:/cache");
    }

    struct fs_file_t f;
    fs_file_t_init(&f);
    int res = fs_open(&f, cache_path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
    if (res != 0) {
        LOG_ERR("Failed to save index cache: %s (err %d)", cache_path, res);
        return;
    }

    EpubCacheHeader hdr = {
        .magic = EPUB_CACHE_MAGIC,
        .version = EPUB_CACHE_VERSION,
        .total_book_text_len = book->total_book_text_len,
        .chapter_count = (uint16_t)book->chapter_count,
        .reserved = 0,
        .cover = book->cover
    };
    strncpy(hdr.title, book->title, sizeof(hdr.title) - 1);
    strncpy(hdr.author, book->author, sizeof(hdr.author) - 1);

    fs_write(&f, &hdr, sizeof(hdr));
    fs_write(&f, book->chapters, sizeof(EpubChapterInfo) * book->chapter_count);
    fs_close(&f);

    LOG_INF("Saved index cache: %s", cache_path);
}

typedef struct {
    struct fs_file_t *src;
    uint32_t remaining;  // compressed bytes left in the current ZIP entry
} EntryReader;

static int entry_read(void *ctx, uint8_t *buf, size_t len)
{
    EntryReader *r = ctx;
    if (r->remaining == 0) return 0;
    if (len > r->remaining) len = r->remaining;
    ssize_t n = fs_read(r->src, buf, len);
    if (n > 0) r->remaining -= (uint32_t)n;
    return (int)n;
}

static int html_sink(void *ctx, const uint8_t *buf, size_t len)
{
    ARG_UNUSED(ctx);
    html_text_feed(&s_html, buf, len);
    return s_html.error;
}

static int text_file_sink(void *ctx, const char *buf, size_t len)
{
    return fs_write((struct fs_file_t *)ctx, buf, len) == (ssize_t)len ? 0 : -EIO;
}

// Streams every chapter through inflate + HTML stripping into one plain-text file
// and records each chapter's byte range in that file.
static bool convert_book_text(const char *epub_path, EpubBook *book)
{
    char text_path[256];
    epub_build_text_path(epub_path, text_path, sizeof(text_path));

    struct fs_file_t src, dst;
    fs_file_t_init(&src);
    fs_file_t_init(&dst);

    int res = fs_open(&src, epub_path, FS_O_READ);
    if (res != 0) {
        LOG_ERR("Failed to open EPUB file: %s (err %d)", epub_path, res);
        return false;
    }
    struct fs_dirent cache_dir_stat;
    if (fs_stat("/SD:/cache", &cache_dir_stat) != 0) {
        fs_mkdir("/SD:/cache");
    }
    res = fs_open(&dst, text_path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
    if (res != 0) {
        LOG_ERR("Failed to create text cache: %s (err %d)", text_path, res);
        fs_close(&src);
        return false;
    }

    uint32_t start_ms = k_uptime_get_32();
    html_text_init(&s_html, text_file_sink, &dst);
    if (s_progress_cb) s_progress_cb(0);

    for (int i = 0; i < book->chapter_count && !s_html.error; i++) {
        EpubChapterInfo *ch = &book->chapters[i];
        html_text_document_break(&s_html);
        ch->cum_offset = html_text_total(&s_html);

        EntryReader reader = { .src = &src, .remaining = ch->compressed_size };
        int err = fs_seek(&src, ch->payload_offset, FS_SEEK_SET);
        if (err == 0 && ch->compression_method == 8) {
            err = inflate_stream(s_work.text.window, s_work.text.in, sizeof(s_work.text.in),
                                 entry_read, html_sink, &reader);
        } else if (err == 0 && ch->compression_method == 0) {
            int n;
            while ((n = entry_read(&reader, s_work.text.in, sizeof(s_work.text.in))) > 0) {
                html_text_feed(&s_html, s_work.text.in, (size_t)n);
            }
            err = n;
        } else if (err == 0) {
            err = -ENOTSUP;
        }
        if (err != 0) {
            // Whatever decoded before the error is kept
            LOG_WRN("Chapter %s: decode error %d", ch->chapter_path, err);
        }

        if (s_progress_cb) s_progress_cb(((i + 1) * 100) / book->chapter_count);
    }

    int write_err = html_text_finish(&s_html);
    uint32_t total = html_text_total(&s_html);
    fs_close(&dst);
    fs_close(&src);

    if (write_err) {
        LOG_ERR("Failed writing text cache: %s", text_path);
        fs_unlink(text_path);
        return false;
    }

    for (int i = 0; i < book->chapter_count; i++) {
        uint32_t end = (i + 1 < book->chapter_count) ? book->chapters[i + 1].cum_offset : total;
        book->chapters[i].text_length = end - book->chapters[i].cum_offset;
    }
    book->total_book_text_len = total;

    LOG_INF("Converted %s -> %s (%u bytes, %u ms)", epub_path, text_path, total,
            k_uptime_get_32() - start_ms);
    return true;
}

typedef struct {
    size_t len;
} CountSink;

static int count_sink(void *ctx, const uint8_t *buf, size_t len) {
    ARG_UNUSED(buf);
    ((CountSink *)ctx)->len += len;
    return 0;
}

typedef struct {
    EntryReader reader;  // first: entry_read() is handed this struct
    CountSink sink;
} SmallEntryCtx;

static int small_entry_read(void *ctx, uint8_t *buf, size_t len) {
    return entry_read(&((SmallEntryCtx *)ctx)->reader, buf, len);
}

static int small_entry_write(void *ctx, const uint8_t *buf, size_t len) {
    return count_sink(&((SmallEntryCtx *)ctx)->sink, buf, len);
}

// Loads a small ZIP entry (OPF, NCX) as a NUL-terminated string in the conversion work area,
// so scanning needs no heap. Output under 32 KB never wraps the inflate history window, so
// the window itself ends up holding the whole entry. Valid until the work area is reused.
static char *load_small_entry(const char *epub_path, uint32_t offset, uint16_t method,
                              uint32_t comp_size, uint32_t uncomp_size) {
    if (uncomp_size == 0 || uncomp_size >= INFLATE_WINDOW_SIZE) return NULL;

    struct fs_file_t file;
    fs_file_t_init(&file);
    if (fs_open(&file, epub_path, FS_O_READ) != 0) return NULL;

    char *out = (char *)s_work.text.window;
    size_t len = 0;
    bool ok = false;
    if (fs_seek(&file, offset, FS_SEEK_SET) == 0) {
        if (method == 0) {
            ssize_t n = fs_read(&file, out, uncomp_size);
            ok = (n == (ssize_t)uncomp_size);
            len = ok ? (size_t)n : 0;
        } else if (method == 8) {
            SmallEntryCtx ctx = { .reader = { .src = &file, .remaining = comp_size } };
            ok = inflate_stream(s_work.text.window, s_work.text.in, sizeof(s_work.text.in),
                                small_entry_read, small_entry_write, &ctx) == 0 &&
                 ctx.sink.len < INFLATE_WINDOW_SIZE;
            len = ok ? ctx.sink.len : 0;
        }
    }
    fs_close(&file);
    if (!ok) return NULL;
    out[len] = '\0';
    return out;
}

// Copies the text inside the first <tag ...>text</...> (or <alt_tag ...>) into out
static void extract_tag_text(const char *xml, const char *tag, const char *alt_tag, char *out, size_t out_size) {
    const char *start = strstr(xml, tag);
    if (!start) start = strstr(xml, alt_tag);
    if (!start) return;
    const char *gt = strchr(start, '>');
    const char *lt = gt ? strchr(gt + 1, '<') : NULL;
    if (!lt || lt <= gt + 1) return;
    size_t len = MIN((size_t)(lt - (gt + 1)), out_size - 1);
    memcpy(out, gt + 1, len);
    out[len] = '\0';
}

bool epub_scan_chapters(const char *epub_path, EpubBook *book) {
    if (!epub_path || !book) return false;

    memset(book, 0, sizeof(EpubBook));
    snprintf(book->book_path, sizeof(book->book_path), "%s", epub_path);

    char cache_path[256];
    build_sidecar_path(epub_path, "idx", cache_path, sizeof(cache_path));

    if (load_sidecar_cache(cache_path, book)) {
        // The index is written after the text, but check the text survived intact
        char text_path[256];
        struct fs_dirent text_stat;
        epub_build_text_path(epub_path, text_path, sizeof(text_path));
        if (fs_stat(text_path, &text_stat) == 0 && text_stat.size == book->total_book_text_len) {
            return true;
        }
        LOG_WRN("Text cache missing or incomplete, rebuilding: %s", text_path);
        memset(book, 0, sizeof(EpubBook));
        snprintf(book->book_path, sizeof(book->book_path), "%s", epub_path);
    }

    struct fs_file_t file;
    fs_file_t_init(&file);

    int res = fs_open(&file, epub_path, FS_O_READ);
    if (res != 0) {
        LOG_ERR("Failed to open EPUB file: %s (err %d)", epub_path, res);
        return false;
    }

    uint32_t curr_pos = 0;
    int scanned_headers = 0;

    uint32_t ncx_payload_offset = 0, ncx_comp_size = 0, ncx_uncomp_size = 0;
    uint16_t ncx_comp_method = 0;

    uint32_t opf_payload_offset = 0, opf_comp_size = 0, opf_uncomp_size = 0;
    uint16_t opf_comp_method = 0;

    while (book->chapter_count < MAX_EPUB_CHAPTERS && scanned_headers < 500) {
        fs_seek(&file, curr_pos, FS_SEEK_SET);

        uint8_t hdr[30];
        ssize_t read_bytes = fs_read(&file, hdr, 30);
        if (read_bytes < 30) {
            break;
        }

        scanned_headers++;

        // Check ZIP Local Header Signature 0x04034b50 ("PK\x03\x04")
        uint32_t sig = hdr[0] | (hdr[1] << 8) | (hdr[2] << 16) | (hdr[3] << 24);
        if (sig != 0x04034b50) {
            break;
        }

        uint16_t comp_method = hdr[8] | (hdr[9] << 8);
        uint32_t comp_size = hdr[18] | (hdr[19] << 8) | (hdr[20] << 16) | (hdr[21] << 24);
        uint32_t uncomp_size = hdr[22] | (hdr[23] << 8) | (hdr[24] << 16) | (hdr[25] << 24);
        uint16_t filename_len = hdr[26] | (hdr[27] << 8);
        uint16_t extra_len = hdr[28] | (hdr[29] << 8);

        char filename[128] = {0};
        if (filename_len < sizeof(filename)) {
            fs_read(&file, filename, filename_len);
        } else {
            fs_seek(&file, filename_len, FS_SEEK_CUR);
        }

        uint32_t payload_offset = curr_pos + 30 + filename_len + extra_len;

        char lower_name[128];
        for (int i = 0; filename[i] && i < 127; i++) {
            lower_name[i] = tolower((unsigned char)filename[i]);
            lower_name[i+1] = '\0';
        }

        if (strstr(lower_name, "toc.ncx") != NULL || strstr(lower_name, ".ncx") != NULL) {
            ncx_payload_offset = payload_offset;
            ncx_comp_method = comp_method;
            ncx_comp_size = comp_size;
            ncx_uncomp_size = uncomp_size;
        }

        bool is_html = (strstr(lower_name, ".xhtml") != NULL || strstr(lower_name, ".html") != NULL || strstr(lower_name, ".htm") != NULL);
        bool is_nav = (strstr(lower_name, "container.xml") != NULL || strstr(lower_name, "content.opf") != NULL ||
                        strstr(lower_name, "toc.ncx") != NULL || strstr(lower_name, "nav.xhtml") != NULL ||
                        strstr(lower_name, "next-reads") != NULL);

        bool is_img = (strstr(lower_name, ".jpg") != NULL || strstr(lower_name, ".jpeg") != NULL || strstr(lower_name, ".png") != NULL);
        if (is_img && (strstr(lower_name, "cover") != NULL || !book->cover.found)) {
            book->cover.found = true;
            snprintf(book->cover.cover_path, sizeof(book->cover.cover_path), "%s", filename);
            book->cover.compression_method = comp_method;
            book->cover.payload_offset = payload_offset;
            book->cover.compressed_size = comp_size;
            book->cover.uncompressed_size = uncomp_size;
        }

        if ((strstr(lower_name, "content.opf") != NULL || strstr(lower_name, ".opf") != NULL) && uncomp_size > 0 && uncomp_size < 32768) {
            opf_payload_offset = payload_offset;
            opf_comp_method = comp_method;
            opf_comp_size = comp_size;
            opf_uncomp_size = uncomp_size;
        }

        if (is_html && !is_nav && uncomp_size > 0) {
            EpubChapterInfo *ch = &book->chapters[book->chapter_count++];
            snprintf(ch->chapter_path, sizeof(ch->chapter_path), "%s", filename);
            ch->compression_method = comp_method;
            ch->payload_offset = payload_offset;
            ch->compressed_size = comp_size;
            ch->uncompressed_size = uncomp_size;
        }

        uint32_t next_pos = payload_offset + comp_size;
        if (next_pos <= curr_pos) {
            LOG_WRN("EPUB corrupt header step (next_pos <= curr_pos) at %u", curr_pos);
            break;
        }
        curr_pos = next_pos;
    }

    fs_close(&file);

    bool spine_ordered = false;
    if (opf_payload_offset > 0) {
        char *opf = load_small_entry(epub_path, opf_payload_offset, opf_comp_method, opf_comp_size, opf_uncomp_size);
        if (opf) {
            extract_tag_text(opf, "<dc:title", "<title", book->title, sizeof(book->title));
            extract_tag_text(opf, "<dc:creator", "<creator", book->author, sizeof(book->author));
            spine_ordered = parse_opf_spine_order(opf, book);
        } else {
            LOG_WRN("[EPUB OPF] Could not read the package file (%u bytes)", opf_uncomp_size);
        }
    }

    if (!spine_ordered && book->chapter_count > 1) {
        qsort(book->chapters, book->chapter_count, sizeof(EpubChapterInfo), natural_chapter_cmp);
    }

    if (ncx_payload_offset > 0) {
        char *ncx = load_small_entry(epub_path, ncx_payload_offset, ncx_comp_method, ncx_comp_size, ncx_uncomp_size);
        if (ncx) {
            parse_toc_ncx_titles(ncx, book);
        } else {
            // Tables of contents over 32 KB don't fit the work area; chapters keep generated names
            LOG_WRN("[EPUB NCX] Table of contents not read (%u bytes)", ncx_uncomp_size);
        }
    }

    int main_chap_counter = 1;
    for (int i = 0; i < book->chapter_count; i++) {
        if (book->chapters[i].title[0] == '\0') {
            char lower_chap[128];
            for (int k = 0; book->chapters[i].chapter_path[k] && k < 127; k++) {
                lower_chap[k] = tolower((unsigned char)book->chapters[i].chapter_path[k]);
                lower_chap[k+1] = '\0';
            }
            if (strstr(lower_chap, "cover") != NULL) {
                snprintf(book->chapters[i].title, sizeof(book->chapters[i].title), "Cover");
            } else if (strstr(lower_chap, "title") != NULL) {
                snprintf(book->chapters[i].title, sizeof(book->chapters[i].title), "Title Page");
            } else if (strstr(lower_chap, "copyright") != NULL) {
                snprintf(book->chapters[i].title, sizeof(book->chapters[i].title), "Copyright");
            } else if (strstr(lower_chap, "foreword") != NULL) {
                snprintf(book->chapters[i].title, sizeof(book->chapters[i].title), "Foreword");
            } else if (strstr(lower_chap, "preface") != NULL) {
                snprintf(book->chapters[i].title, sizeof(book->chapters[i].title), "Preface");
            } else if (strstr(lower_chap, "prologue") != NULL) {
                snprintf(book->chapters[i].title, sizeof(book->chapters[i].title), "Prologue");
            } else if (strstr(lower_chap, "epilogue") != NULL) {
                snprintf(book->chapters[i].title, sizeof(book->chapters[i].title), "Epilogue");
            } else if (strstr(lower_chap, "ack") != NULL) {
                snprintf(book->chapters[i].title, sizeof(book->chapters[i].title), "Acknowledgements");
            } else if (strstr(lower_chap, "author") != NULL || strstr(lower_chap, "bio") != NULL || strstr(lower_chap, "about") != NULL) {
                snprintf(book->chapters[i].title, sizeof(book->chapters[i].title), "About the Author");
            } else {
                snprintf(book->chapters[i].title, sizeof(book->chapters[i].title), "Chapter %d", main_chap_counter++);
            }
        }
    }

    if (book->chapter_count == 0 || !convert_book_text(epub_path, book)) {
        return false;
    }

    LOG_INF("Indexed \"%s\" by %s: %d chapters%s, %u bytes of text", book->title, book->author,
            book->chapter_count, spine_ordered ? " (spine order)" : "", book->total_book_text_len);

    save_sidecar_cache(cache_path, book);
    return true;
}

#define COVER_TMP_PATH "/SD:/cache/cover.tmp"

typedef struct CoverJpeg CoverJpeg;
// Receives each full output row (target_w pixels, white margins included) in order
typedef void (*cover_row_fn)(CoverJpeg *cj, const uint8_t *row);

struct CoverJpeg {
    struct fs_file_t *file;
    uint32_t remaining;  // JPEG bytes left in the stream
    int target_w, target_h;
    int dec_w, dec_h;    // TJpgDec output size at the chosen scale
    int fit_w, fit_h;    // size of the cover inside the target
    int off_x, off_y;
    int rows_done;       // fit rows finished
    int rows_emitted;    // target rows handed to emit
    cover_row_fn emit;
    Ditherer dither;     // pass 2
    uint8_t *bits;       // pass 2 output, (target_w + 7) / 8 bytes per row
};

static size_t cover_jpeg_in(JDEC *jd, uint8_t *buf, size_t len)
{
    CoverJpeg *cj = jd->device;
    if (len > cj->remaining) len = cj->remaining;
    if (len == 0) return 0;
    if (!buf) {
        // NULL buffer: TJpgDec is skipping a segment it does not need (EXIF, ICC, ...)
        if (fs_seek(cj->file, (off_t)len, FS_SEEK_CUR) != 0) return 0;
        cj->remaining -= (uint32_t)len;
        return len;
    }
    ssize_t n = fs_read(cj->file, buf, len);
    if (n <= 0) return 0;
    cj->remaining -= (uint32_t)n;
    return (size_t)n;
}

// Number of decoded pixels that land in output pixel o when d pixels shrink to f (d >= f)
static uint32_t cover_span(uint32_t o, uint32_t d, uint32_t f)
{
    return ((o + 1) * d + f - 1) / f - (o * d + f - 1) / f;
}

// White rows above and below the fitted cover
static void cover_emit_white_until(CoverJpeg *cj, int target_row) {
    uint8_t *row = s_work.cover.row;
    for (; cj->rows_emitted < target_row; cj->rows_emitted++) {
        memset(row, 0xFF, cj->target_w);
        cj->emit(cj, row);
    }
}

static void cover_flush_rows(CoverJpeg *cj, int upto)
{
    uint8_t *row = s_work.cover.row;
    for (; cj->rows_done < upto; cj->rows_done++) {
        int oy = cj->rows_done;
        cover_emit_white_until(cj, cj->off_y + oy);
        uint16_t *acc = s_work.cover.acc[oy % COVER_ACC_ROWS];
        uint32_t ny = cover_span(oy, cj->dec_h, cj->fit_h);
        memset(row, 0xFF, cj->target_w);
        for (int ox = 0; ox < cj->fit_w; ox++) {
            uint32_t n = ny * cover_span(ox, cj->dec_w, cj->fit_w);
            row[cj->off_x + ox] = (uint8_t)((acc[ox] + n / 2) / n);
            acc[ox] = 0;
        }
        cj->emit(cj, row);
        cj->rows_emitted++;
    }
}

// Box-filters each decoded block into the output rows it covers. MCUs arrive in raster
// order, so a row is final once the MCU row containing its last source line is done.
static int cover_jpeg_out(JDEC *jd, void *bitmap, JRECT *rect)
{
    CoverJpeg *cj = jd->device;
    const uint8_t *px = bitmap;
    for (int y = rect->top; y <= rect->bottom; y++) {
        uint16_t *acc = s_work.cover.acc[(y * cj->fit_h / cj->dec_h) % COVER_ACC_ROWS];
        for (int x = rect->left; x <= rect->right; x++) {
            acc[x * cj->fit_w / cj->dec_w] += *px++;
        }
    }
    if (rect->right + 1 >= cj->dec_w) {
        cover_flush_rows(cj, (rect->bottom + 1) * cj->fit_h / cj->dec_h);
    }
    return 1;
}

// Decodes the baseline JPEG at the file's current position, fitted whole and centred on
// white, handing each of the target_h rows to cj->emit in order.
static bool decode_cover_jpeg(CoverJpeg *cj_in, uint32_t size)
{
    struct fs_file_t *file = cj_in->file;
    int target_w = cj_in->target_w;
    int target_h = cj_in->target_h;
    if (target_w > COVER_ACC_MAX_W) return false;

    // TJpgDec hunts for SOI byte by byte, so reject non-JPEG data up front
    uint8_t soi[2];
    off_t start = fs_tell(file);
    if (size < 4 || fs_read(file, soi, 2) != 2 || soi[0] != 0xFF || soi[1] != 0xD8 ||
        fs_seek(file, start, FS_SEEK_SET) != 0) {
        LOG_WRN("Cover is not a JPEG");
        return false;
    }

    CoverJpeg cj = *cj_in;
    cj.remaining = size;
    cj.rows_done = 0;
    cj.rows_emitted = 0;
    JDEC jd;
    JRESULT rc = jd_prepare(&jd, cover_jpeg_in, s_work.cover.pool, sizeof(s_work.cover.pool), &cj);
    if (rc != JDR_OK) {
        // JDR_FMT3 covers progressive and other non-baseline JPEGs
        LOG_WRN("Cover JPEG not decodable (jd_prepare %d)", rc);
        return false;
    }

    // Fit the whole cover inside the target, keeping its aspect ratio
    int fit_w = target_w;
    int fit_h = (int)(((uint32_t)jd.height * target_w + jd.width / 2) / jd.width);
    if (fit_h > target_h) {
        fit_h = target_h;
        fit_w = (int)(((uint32_t)jd.width * target_h + jd.height / 2) / jd.height);
    }
    if (fit_w < 1) fit_w = 1;
    if (fit_h < 1) fit_h = 1;

    // Smallest TJpgDec scale that still has at least one pixel per output pixel
    uint8_t scale = 3;
    while (scale > 0 && ((jd.width >> scale) < fit_w || (jd.height >> scale) < fit_h)) scale--;
    cj.dec_w = jd.width >> scale;
    cj.dec_h = jd.height >> scale;
    // Tiny images are centred at their own size rather than upscaled
    if (cj.dec_w < fit_w) fit_w = cj.dec_w;
    if (cj.dec_h < fit_h) fit_h = cj.dec_h;
    // Each 16-bit sum must hold every source pixel of its output pixel
    uint32_t max_px = (uint32_t)((cj.dec_w + fit_w - 1) / fit_w) * ((cj.dec_h + fit_h - 1) / fit_h);
    if (max_px > 0xFFFF / 255) {
        LOG_WRN("Cover JPEG too large: %ux%u", jd.width, jd.height);
        return false;
    }
    cj.fit_w = fit_w;
    cj.fit_h = fit_h;
    cj.off_x = (target_w - fit_w) / 2;
    cj.off_y = (target_h - fit_h) / 2;

    memset(s_work.cover.acc, 0, sizeof(s_work.cover.acc));

    uint32_t start_ms = k_uptime_get_32();
    rc = jd_decomp(&jd, cover_jpeg_out, scale);
    if (rc != JDR_OK) {
        LOG_WRN("Cover JPEG decode failed (jd_decomp %d)", rc);
        return false;
    }
    cover_flush_rows(&cj, fit_h);
    cover_emit_white_until(&cj, target_h);
    LOG_INF("Cover JPEG %ux%u decoded at 1/%d to %dx%d in %u ms", jd.width, jd.height,
            1 << scale, fit_w, fit_h, k_uptime_get_32() - start_ms);
    return true;
}

static void emit_hist_row(CoverJpeg *cj, const uint8_t *row) {
    dither_hist_row(s_work.cover.hist, row, cj->target_w);
}

static void emit_dither_row(CoverJpeg *cj, const uint8_t *row) {
    size_t stride = ((size_t)cj->target_w + 7) / 8;
    dither_row(&cj->dither, row, cj->bits + (size_t)cj->rows_emitted * stride);
}

// Two decode passes keep RAM down to a few rows: the first only measures the tone range,
// the second dithers each row straight into the 1-bit output. Only the first lock screen
// of a book pays for this; the result is cached.
static bool decode_cover_bits(struct fs_file_t *file, uint32_t size, uint8_t *bits, int w, int h) {
    off_t start = fs_tell(file);
    CoverJpeg cj = { .file = file, .target_w = w, .target_h = h, .emit = emit_hist_row };
    memset(s_work.cover.hist, 0, sizeof(s_work.cover.hist));
    if (!decode_cover_jpeg(&cj, size)) return false;

    dither_build_lut(s_work.cover.hist, s_work.cover.lut);
    dither_start(&cj.dither, w, s_work.cover.err, s_work.cover.lut);
    cj.emit = emit_dither_row;
    cj.bits = bits;
    return fs_seek(file, start, FS_SEEK_SET) == 0 && decode_cover_jpeg(&cj, size);
}

typedef struct {
    EntryReader reader;  // first, so entry_read() can take this context as-is
    struct fs_file_t *dst;
} CoverInflate;

static int cover_tmp_sink(void *ctx, const uint8_t *buf, size_t len)
{
    CoverInflate *ci = ctx;
    return fs_write(ci->dst, buf, len) == (ssize_t)len ? 0 : -EIO;
}

// TJpgDec pulls its input while inflate pushes its output, and the 32 KB inflate window
// leaves no room for the JPEG pool, so deflated covers are unpacked to a temp file first.
static bool inflate_cover_to_tmp(struct fs_file_t *src, const EpubCoverInfo *cover)
{
    struct fs_file_t dst;
    fs_file_t_init(&dst);
    if (fs_open(&dst, COVER_TMP_PATH, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC) != 0) {
        return false;
    }
    CoverInflate ci = { .reader = { .src = src, .remaining = cover->compressed_size }, .dst = &dst };
    int err = fs_seek(src, cover->payload_offset, FS_SEEK_SET);
    if (err == 0) {
        err = inflate_stream(s_work.text.window, s_work.text.in, sizeof(s_work.text.in),
                             entry_read, cover_tmp_sink, &ci);
    }
    fs_close(&dst);
    if (err != 0) {
        LOG_WRN("Cover inflate failed (%d)", err);
    }
    return err == 0;
}

// .cov sidecar: header + the dithered 1-bit cover (rows of (w + 7) / 8 bytes, 1 = white), or
// a header with 0x0 size meaning "this book's cover can't be decoded" so the slow path isn't
// retried on every lock screen. Older formats (headerless, or COV2 8-bit grayscale) are
// discarded and decoded again.
#define COVER_CACHE_MAGIC 0x33564F43u  /* "COV3" */

static size_t cover_bits_size(int w, int h) {
    return (((size_t)w + 7) / 8) * (size_t)h;
}

typedef struct {
    uint32_t magic;
    uint16_t width;
    uint16_t height;
} CoverCacheHeader;

static void save_cover_cache(const char *cov_path, const uint8_t *bits, int w, int h) {
    struct fs_file_t out_file;
    fs_file_t_init(&out_file);
    int res = fs_open(&out_file, cov_path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
    if (res != 0) {
        LOG_ERR("Failed to create cover cache bitmap: %s (err %d)", cov_path, res);
        return;
    }
    CoverCacheHeader hdr = { .magic = COVER_CACHE_MAGIC, .width = (uint16_t)w, .height = (uint16_t)h };
    fs_write(&out_file, &hdr, sizeof(hdr));
    if (bits) {
        fs_write(&out_file, bits, cover_bits_size(w, h));
    }
    fs_close(&out_file);
    LOG_INF("Saved cover cache: %s (%dx%d)", cov_path, w, h);
}

// Returns 1 = cover loaded, 0 = cached "no cover", -1 = no usable cache
static int load_cover_cache(const char *cov_path, uint8_t *bits_out, int w, int h) {
    struct fs_file_t cov_file;
    fs_file_t_init(&cov_file);
    if (fs_open(&cov_file, cov_path, FS_O_READ) != 0) {
        return -1;
    }
    CoverCacheHeader hdr;
    int result = -1;
    if (fs_read(&cov_file, &hdr, sizeof(hdr)) == (ssize_t)sizeof(hdr) && hdr.magic == COVER_CACHE_MAGIC) {
        if (hdr.width == 0 && hdr.height == 0) {
            result = 0;
        } else if (hdr.width == w && hdr.height == h &&
                   fs_read(&cov_file, bits_out, cover_bits_size(w, h)) == (ssize_t)cover_bits_size(w, h)) {
            result = 1;
        }
    }
    fs_close(&cov_file);
    if (result < 0) {
        LOG_INF("Discarding stale cover cache: %s", cov_path);
        fs_unlink(cov_path);
    }
    return result;
}

bool epub_extract_cover_image(const EpubBook *book, uint8_t *bits_out, int target_w, int target_h) {
    if (!book || !bits_out || target_w <= 0 || target_h <= 0) return false;

    /* 1. Fast Path: pre-rendered cover bitmap (or a cached "no cover") on the SD card */
    char cov_path[256];
    build_sidecar_path(book->book_path, "cov", cov_path, sizeof(cov_path));

    int cached = load_cover_cache(cov_path, bits_out, target_w, target_h);
    if (cached >= 0) {
        LOG_INF("Cover from cache: %s (%s)", cov_path, cached ? "image" : "none");
        return cached == 1;
    }

    /* 2. Slow Path: decode the cover JPEG straight from the EPUB (or an inflated temp copy) */
    if (!book->cover.found) {
        LOG_WRN("No cover image entry found in EPUB: %s", book->book_path);
        return false;
    }
    if (my_strcasestr(book->cover.cover_path, ".png") != NULL) {
        LOG_INF("PNG cover not supported: %s", book->cover.cover_path);
        save_cover_cache(cov_path, NULL, 0, 0);
        return false;
    }
    uint16_t comp_method = book->cover.compression_method;
    if (comp_method != 0 && comp_method != 8) {
        LOG_WRN("Unsupported cover compression %u: %s", comp_method, book->cover.cover_path);
        save_cover_cache(cov_path, NULL, 0, 0);
        return false;
    }

    struct fs_dirent cache_dir_stat;
    if (fs_stat("/SD:/cache", &cache_dir_stat) != 0) {
        fs_mkdir("/SD:/cache");
    }

    struct fs_file_t file;
    fs_file_t_init(&file);
    if (fs_open(&file, book->book_path, FS_O_READ) != 0) {
        return false;
    }

    bool ok;
    if (comp_method == 0) {
        ok = fs_seek(&file, book->cover.payload_offset, FS_SEEK_SET) == 0 &&
             decode_cover_bits(&file, book->cover.compressed_size, bits_out, target_w, target_h);
        fs_close(&file);
    } else {
        ok = inflate_cover_to_tmp(&file, &book->cover);
        fs_close(&file);
        if (ok) {
            fs_file_t_init(&file);
            ok = fs_open(&file, COVER_TMP_PATH, FS_O_READ) == 0;
            if (ok) {
                ok = decode_cover_bits(&file, book->cover.uncompressed_size, bits_out, target_w, target_h);
                fs_close(&file);
            }
        }
        fs_unlink(COVER_TMP_PATH);
    }
    /* 3. Cache the result (bitmap, or "no cover" for e.g. progressive JPEGs) for future lock screens */
    if (ok) {
        save_cover_cache(cov_path, bits_out, target_w, target_h);
    } else {
        save_cover_cache(cov_path, NULL, 0, 0);
    }
    return ok;
}
