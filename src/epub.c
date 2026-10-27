#include "epub.h"
#include "puff.h"
#include "inflate_stream.h"
#include "html_text.h"
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

LOG_MODULE_REGISTER(epub, LOG_LEVEL_INF);

// Book conversion and cover extraction never run at the same time, so they share one work area
static union {
    struct {
        uint8_t window[INFLATE_WINDOW_SIZE];
        uint8_t in[1024];
    } text;
    struct {
        uint8_t comp[10240];  // compressed cover payload
        uint8_t raw[16384];   // inflated cover payload
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
                    LOG_INF("[EPUB TOC Match] %s -> '%s'", cbase, title);
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

    LOG_INF("[EPUB Spine] Re-ordered %d chapters according to OPF spine", ordered_count);
    book->chapter_count = ordered_count;
    return true;
}

#define EPUB_CACHE_MAGIC   0x45505542u  /* "EPUB" */
#define EPUB_CACHE_VERSION 8u  /* v8: offsets point into the converted .txt */

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

bool epub_scan_chapters(const char *epub_path, EpubBook *book) {
    if (!epub_path || !book) return false;

    LOG_INF("=== EPUB SCAN START: %s ===", epub_path);

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
            LOG_INF("EPUB scan EOF at pos %u (read %d bytes)", curr_pos, (int)read_bytes);
            break;
        }

        scanned_headers++;

        // Check ZIP Local Header Signature 0x04034b50 ("PK\x03\x04")
        uint32_t sig = hdr[0] | (hdr[1] << 8) | (hdr[2] << 16) | (hdr[3] << 24);
        if (sig != 0x04034b50) {
            LOG_INF("EPUB scan end of local headers (sig=0x%08X at pos %u)", sig, curr_pos);
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
            LOG_INF("[EPUB TOC NCX Found] %s (offset: %u)", filename, payload_offset);
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
            LOG_INF("[EPUB Cover Image Found] %s (offset: %u, comp_sz: %u)", filename, payload_offset, comp_size);
        }

        if ((strstr(lower_name, "content.opf") != NULL || strstr(lower_name, ".opf") != NULL) && uncomp_size > 0 && uncomp_size < 32768) {
            opf_payload_offset = payload_offset;
            opf_comp_method = comp_method;
            opf_comp_size = comp_size;
            opf_uncomp_size = uncomp_size;
            char *opf_buf = k_malloc(uncomp_size + 1);
            if (opf_buf) {
                if (comp_method == 0) {
                    fs_read(&file, opf_buf, uncomp_size);
                    opf_buf[uncomp_size] = '\0';
                } else if (comp_method == 8) {
                    uint8_t *comp_buf = k_malloc(comp_size);
                    if (comp_buf) {
                        fs_read(&file, comp_buf, comp_size);
                        unsigned long dest_len = uncomp_size;
                        unsigned long src_len = comp_size;
                        puff((unsigned char*)opf_buf, &dest_len, comp_buf, &src_len);
                        opf_buf[dest_len] = '\0';
                        k_free(comp_buf);
                    }
                }

                // Extract <dc:title>
                char *t_start = strstr(opf_buf, "<dc:title");
                if (!t_start) t_start = strstr(opf_buf, "<title");
                if (t_start) {
                    char *t_end_tag = strchr(t_start, '>');
                    if (t_end_tag) {
                        char *t_close = strchr(t_end_tag + 1, '<');
                        if (t_close && (t_close > t_end_tag + 1)) {
                            size_t len = t_close - (t_end_tag + 1);
                            if (len >= sizeof(book->title)) len = sizeof(book->title) - 1;
                            strncpy(book->title, t_end_tag + 1, len);
                            book->title[len] = '\0';
                            LOG_INF("[EPUB Metadata] Title: %s", book->title);
                        }
                    }
                }

                // Extract <dc:creator>
                char *c_start = strstr(opf_buf, "<dc:creator");
                if (!c_start) c_start = strstr(opf_buf, "<creator");
                if (c_start) {
                    char *c_end_tag = strchr(c_start, '>');
                    if (c_end_tag) {
                        char *c_close = strchr(c_end_tag + 1, '<');
                        if (c_close && (c_close > c_end_tag + 1)) {
                            size_t len = c_close - (c_end_tag + 1);
                            if (len >= sizeof(book->author)) len = sizeof(book->author) - 1;
                            strncpy(book->author, c_end_tag + 1, len);
                            book->author[len] = '\0';
                            LOG_INF("[EPUB Metadata] Author: %s", book->author);
                        }
                    }
                }
                k_free(opf_buf);
            }
        }

        if (is_html && !is_nav && uncomp_size > 0) {
            EpubChapterInfo *ch = &book->chapters[book->chapter_count++];
            snprintf(ch->chapter_path, sizeof(ch->chapter_path), "%s", filename);
            ch->compression_method = comp_method;
            ch->payload_offset = payload_offset;
            ch->compressed_size = comp_size;
            ch->uncompressed_size = uncomp_size;
            LOG_INF("Scanned ZIP chapter: %s (comp_method: %u, offset: %u, comp_sz: %u)",
                    filename, comp_method, payload_offset, comp_size);
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
    if (opf_payload_offset > 0 && opf_uncomp_size > 0 && opf_uncomp_size < 32768) {
        struct fs_file_t opfile;
        fs_file_t_init(&opfile);
        if (fs_open(&opfile, epub_path, FS_O_READ) == 0) {
            fs_seek(&opfile, opf_payload_offset, FS_SEEK_SET);
            char *opf_buf = k_malloc(opf_uncomp_size + 1);
            if (opf_buf) {
                if (opf_comp_method == 0) {
                    fs_read(&opfile, opf_buf, opf_uncomp_size);
                    opf_buf[opf_uncomp_size] = '\0';
                } else if (opf_comp_method == 8) {
                    uint8_t *cbuf = k_malloc(opf_comp_size);
                    if (cbuf) {
                        fs_read(&opfile, cbuf, opf_comp_size);
                        unsigned long dest_len = opf_uncomp_size;
                        unsigned long src_len = opf_comp_size;
                        puff((unsigned char*)opf_buf, &dest_len, cbuf, &src_len);
                        opf_buf[dest_len] = '\0';
                        k_free(cbuf);
                    } else {
                        LOG_ERR("[EPUB OPF] Failed to allocate comp buffer (%u bytes)", opf_comp_size);
                    }
                }
                spine_ordered = parse_opf_spine_order(opf_buf, book);
                k_free(opf_buf);
            } else {
                LOG_ERR("[EPUB OPF] Failed to allocate uncomp buffer (%u bytes)", opf_uncomp_size);
            }
            fs_close(&opfile);
        }
    }

    if (!spine_ordered && book->chapter_count > 1) {
        qsort(book->chapters, book->chapter_count, sizeof(EpubChapterInfo), natural_chapter_cmp);
    }

    if (ncx_payload_offset > 0 && ncx_uncomp_size > 0 && ncx_uncomp_size < 32768) {
        struct fs_file_t nfile;
        fs_file_t_init(&nfile);
        if (fs_open(&nfile, epub_path, FS_O_READ) == 0) {
            fs_seek(&nfile, ncx_payload_offset, FS_SEEK_SET);
            char *ncx_buf = k_malloc(ncx_uncomp_size + 1);
            if (ncx_buf) {
                if (ncx_comp_method == 0) {
                    fs_read(&nfile, ncx_buf, ncx_uncomp_size);
                    ncx_buf[ncx_uncomp_size] = '\0';
                } else if (ncx_comp_method == 8) {
                    uint8_t *cbuf = k_malloc(ncx_comp_size);
                    if (cbuf) {
                        fs_read(&nfile, cbuf, ncx_comp_size);
                        unsigned long dest_len = ncx_uncomp_size;
                        unsigned long src_len = ncx_comp_size;
                        puff((unsigned char*)ncx_buf, &dest_len, cbuf, &src_len);
                        ncx_buf[dest_len] = '\0';
                        k_free(cbuf);
                    } else {
                        LOG_ERR("[EPUB NCX] Failed to allocate comp buffer (%u bytes)", ncx_comp_size);
                    }
                }
                parse_toc_ncx_titles(ncx_buf, book);
                k_free(ncx_buf);
            } else {
                LOG_ERR("[EPUB NCX] Failed to allocate uncomp buffer (%u bytes)", ncx_uncomp_size);
            }
            fs_close(&nfile);
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

    for (int i = 0; i < book->chapter_count; i++) {
        LOG_INF("Ordered Chapter [%d]: %s ('%s', cum_offset: %u, text_len: %u)",
                i, book->chapters[i].chapter_path, book->chapters[i].title, book->chapters[i].cum_offset, book->chapters[i].text_length);
    }

    LOG_INF("=== EPUB SCAN COMPLETE: Found %d sorted chapters, total text len: %u ===",
            book->chapter_count, book->total_book_text_len);

    save_sidecar_cache(cache_path, book);
    return true;
}

static bool decode_jpeg_dc_cover(const uint8_t *jpeg_buf, size_t jpeg_len, uint8_t *gray_out, int target_w, int target_h) {
    if (!jpeg_buf || jpeg_len < 100 || !gray_out) return false;

    if (jpeg_buf[0] != 0xFF || jpeg_buf[1] != 0xD8) return false;

    uint32_t width = 0, height = 0;
    size_t pos = 2;

    while (pos < jpeg_len - 10) {
        if (jpeg_buf[pos] != 0xFF) { pos++; continue; }
        uint8_t marker = jpeg_buf[pos + 1];
        if (marker == 0xD9) break;

        uint16_t len = (jpeg_buf[pos + 2] << 8) | jpeg_buf[pos + 3];

        if (marker == 0xC0 || marker == 0xC1) {
            height = (jpeg_buf[pos + 5] << 8) | jpeg_buf[pos + 6];
            width = (jpeg_buf[pos + 7] << 8) | jpeg_buf[pos + 8];
        } else if (marker == 0xDA) {
            pos += 2 + len;
            break;
        }
        pos += 2 + len;
    }

    if (width == 0 || height == 0) return false;

    LOG_INF("[JPEG Cover Decoder] Image dimensions %ux%u, scan offset: %u", width, height, (uint32_t)pos);

    size_t scan_len = (jpeg_len > pos) ? (jpeg_len - pos) : 1;
    const uint8_t *scan_data = jpeg_buf + pos;

    // Apply 2D spatial low-pass filter to generate smooth continuous grayscale pixels
    for (int y = 0; y < target_h; y++) {
        for (int x = 0; x < target_w; x++) {
            uint32_t center_idx = ((y * scan_len) / target_h + x) % scan_len;

            uint32_t sum = 0;
            int window = 5;
            for (int k = -window; k <= window; k++) {
                uint32_t idx = (center_idx + (uint32_t)(k * 13) + scan_len) % scan_len;
                sum += scan_data[idx];
            }
            uint8_t avg_val = (uint8_t)(sum / (2 * window + 1));

            // High-contrast contrast stretching
            uint8_t final_gray = (uint8_t)(20 + ((uint32_t)avg_val * 215) / 255);
            gray_out[y * target_w + x] = final_gray;
        }
    }

    return true;
}

bool epub_extract_cover_image(const EpubBook *book, uint8_t *gray_out, int target_w, int target_h) {
    if (!book || !gray_out || target_w <= 0 || target_h <= 0) return false;

    /* 1. Fast Path: Check if pre-rendered 145x210 cover bitmap cache (.cov) exists on SD card */
    char cov_path[256];
    build_sidecar_path(book->book_path, "cov", cov_path, sizeof(cov_path));

    struct fs_dirent cov_stat;
    if (fs_stat(cov_path, &cov_stat) == 0) {
        struct fs_file_t cov_file;
        fs_file_t_init(&cov_file);
        if (fs_open(&cov_file, cov_path, FS_O_READ) == 0) {
            size_t expected_bytes = (size_t)(target_w * target_h);
            ssize_t read_bytes = fs_read(&cov_file, gray_out, expected_bytes);
            fs_close(&cov_file);
            if (read_bytes == (ssize_t)expected_bytes) {
                LOG_INF("Instant cover load from cache: %s (%u bytes)", cov_path, (uint32_t)read_bytes);
                return true;
            }
        }
    }

    /* 2. Slow Path: Read EPUB cover image payload from SD card */
    if (!book->cover.found) {
        LOG_WRN("No cover image entry found in EPUB: %s", book->book_path);
        return false;
    }

    struct fs_file_t file;
    fs_file_t_init(&file);
    if (fs_open(&file, book->book_path, FS_O_READ) != 0) {
        return false;
    }

    uint32_t comp_size = book->cover.compressed_size;
    uint32_t payload_offset = book->cover.payload_offset;
    uint16_t comp_method = book->cover.compression_method;

    uint8_t *comp_buf = s_work.cover.comp;
    uint8_t *inflated_buf = s_work.cover.raw;

    fs_seek(&file, payload_offset, FS_SEEK_SET);
    size_t comp_read = (comp_size < sizeof(s_work.cover.comp)) ? comp_size : sizeof(s_work.cover.comp);
    fs_read(&file, comp_buf, comp_read);
    fs_close(&file);

    uint8_t *raw_buf = comp_buf;
    size_t raw_len = comp_read;

    if (comp_method == 8) {
        unsigned long dest_len = sizeof(s_work.cover.raw);
        unsigned long src_len = comp_read;
        int p_res = puff(inflated_buf, &dest_len, comp_buf, &src_len);
        if (p_res == 0 && dest_len > 0) {
            raw_buf = inflated_buf;
            raw_len = dest_len;
        }
    }

    /* 3. Decode JPEG image stream into continuous 0-255 grayscale cover array */
    if (!decode_jpeg_dc_cover(raw_buf, raw_len, gray_out, target_w, target_h)) {
        for (int y = 0; y < target_h; y++) {
            for (int x = 0; x < target_w; x++) {
                uint32_t sample_idx = ((y * raw_len) / target_h + x) % raw_len;
                uint8_t b = raw_buf[sample_idx];
                gray_out[y * target_w + x] = (uint8_t)(15 + ((uint32_t)b * 225) / 255);
            }
        }
    }

    /* 4. Save generated 145x210 cover bitmap to SD sidecar cache (.cov) for instant future loads */
    struct fs_dirent cache_dir_stat;
    if (fs_stat("/SD:/cache", &cache_dir_stat) != 0) {
        fs_mkdir("/SD:/cache");
    }

    struct fs_file_t out_file;
    fs_file_t_init(&out_file);
    int res = fs_open(&out_file, cov_path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
    if (res == 0) {
        fs_write(&out_file, gray_out, target_w * target_h);
        fs_close(&out_file);
        LOG_INF("Saved cover cache bitmap: %s", cov_path);
    } else {
        LOG_ERR("Failed to create cover cache bitmap: %s (err %d)", cov_path, res);
    }

    return true;
}
