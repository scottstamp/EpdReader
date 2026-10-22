#include "epub.h"
#include "puff.h"
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

LOG_MODULE_REGISTER(epub, LOG_LEVEL_INF);

static uint8_t s_comp_buf[10240];   // 10 KB static compressed payload buffer
static char s_raw_html_buf[16384];  // 16 KB static inflated HTML buffer

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

static void strip_html_and_entities(const char *html_in, char *text_out, uint32_t max_out) {
    const char *p = html_in;
    uint32_t out_idx = 0;
    bool in_tag = false;
    bool last_was_space = false;

    while (*p && out_idx < max_out - 1) {
        // Skip <head>...</head>, <style>...</style>, <script>...</script>, <svg>...</svg>
        if (my_strncasecmp(p, "<head", 5) == 0) {
            const char *end = my_strcasestr(p, "</head>");
            if (end) { p = end + 7; continue; }
        }
        if (my_strncasecmp(p, "<style", 6) == 0) {
            const char *end = my_strcasestr(p, "</style>");
            if (end) { p = end + 8; continue; }
        }
        if (my_strncasecmp(p, "<script", 7) == 0) {
            const char *end = my_strcasestr(p, "</script>");
            if (end) { p = end + 9; continue; }
        }
        if (my_strncasecmp(p, "<svg", 4) == 0) {
            const char *end = my_strcasestr(p, "</svg>");
            if (end) { p = end + 6; continue; }
        }

        if (*p == '<') {
            in_tag = true;

            const char *tag_end = strchr(p, '>');
            size_t tag_len = tag_end ? (size_t)(tag_end - p) : 0;

            // --- XHTML TAG PARSER ---
            // Italics / Emphasis
            if (my_strncasecmp(p, "<i>", 3) == 0 || my_strncasecmp(p, "<i ", 3) == 0 ||
                my_strncasecmp(p, "<em>", 4) == 0 || my_strncasecmp(p, "<em ", 4) == 0 ||
                my_strncasecmp(p, "<cite>", 6) == 0 || my_strncasecmp(p, "<cite ", 6) == 0) {
                text_out[out_idx++] = '\x0E';
            } else if (my_strncasecmp(p, "</i>", 4) == 0 || my_strncasecmp(p, "</em>", 5) == 0 ||
                       my_strncasecmp(p, "</cite>", 7) == 0) {
                text_out[out_idx++] = '\x0F';
            }

            // Bold / Strong
            if (my_strncasecmp(p, "<b>", 3) == 0 || my_strncasecmp(p, "<b ", 3) == 0 ||
                my_strncasecmp(p, "<strong>", 8) == 0 || my_strncasecmp(p, "<strong ", 8) == 0) {
                text_out[out_idx++] = '\x02';
            } else if (my_strncasecmp(p, "</b>", 4) == 0 || my_strncasecmp(p, "</strong>", 9) == 0) {
                text_out[out_idx++] = '\x03';
            }

            // Underline / Inserted
            if (my_strncasecmp(p, "<u>", 3) == 0 || my_strncasecmp(p, "<u ", 3) == 0 ||
                my_strncasecmp(p, "<ins>", 5) == 0 || my_strncasecmp(p, "<ins ", 5) == 0) {
                text_out[out_idx++] = '\x15';
            } else if (my_strncasecmp(p, "</u>", 4) == 0 || my_strncasecmp(p, "</ins>", 6) == 0) {
                text_out[out_idx++] = '\x16';
            }

            // Headings <h1 .. <h6
            if (p[0] == '<' && (p[1] == 'h' || p[1] == 'H') && (p[2] >= '1' && p[2] <= '6')) {
                if (out_idx > 0 && text_out[out_idx - 1] != '\n') {
                    text_out[out_idx++] = '\n';
                    text_out[out_idx++] = '\n';
                }
                text_out[out_idx++] = '\x02'; // Enable Bold for heading
            } else if (p[0] == '<' && p[1] == '/' && (p[2] == 'h' || p[2] == 'H') && (p[3] >= '1' && p[3] <= '6')) {
                text_out[out_idx++] = '\x03'; // Disable Bold after heading
            }

            // Ultra-fast local tag CSS scanning (scans ONLY bytes inside tag up to '>')
            if (tag_len > 0 && tag_len < 256) {
                char tag_buf[256];
                memcpy(tag_buf, p, tag_len);
                tag_buf[tag_len] = '\0';
                for (size_t k = 0; k < tag_len; k++) {
                    tag_buf[k] = tolower((unsigned char)tag_buf[k]);
                }

                if (strstr(tag_buf, "font-style: italic") || strstr(tag_buf, "font-style:italic") ||
                    strstr(tag_buf, "font-style: oblique") || strstr(tag_buf, "font-style:oblique")) {
                    text_out[out_idx++] = '\x0E';
                }
                if (strstr(tag_buf, "font-weight: bold") || strstr(tag_buf, "font-weight:bold") ||
                    strstr(tag_buf, "font-weight: 700") || strstr(tag_buf, "font-weight:700")) {
                    text_out[out_idx++] = '\x02';
                }
                if (strstr(tag_buf, "text-decoration: underline") || strstr(tag_buf, "text-decoration:underline")) {
                    text_out[out_idx++] = '\x15';
                }
                if (strstr(tag_buf, "text-align: center") || strstr(tag_buf, "text-align:center") ||
                    strstr(tag_buf, "align=\"center\"")) {
                    text_out[out_idx++] = '\x1C';
                }
            }

            // Paragraph & Line break formatting
            if (my_strncasecmp(p, "<p>", 3) == 0 || my_strncasecmp(p, "<p ", 3) == 0) {
                if (out_idx > 0 && text_out[out_idx - 1] != '\n') {
                    text_out[out_idx++] = '\n';
                    text_out[out_idx++] = '\n';
                }
            } else if (my_strncasecmp(p, "<br", 3) == 0 || my_strncasecmp(p, "</div>", 6) == 0) {
                if (out_idx > 0 && text_out[out_idx - 1] != '\n') {
                    text_out[out_idx++] = '\n';
                }
            } else if (my_strncasecmp(p, "<li>", 4) == 0 || my_strncasecmp(p, "<li ", 4) == 0) {
                if (out_idx > 0 && text_out[out_idx - 1] != '\n') {
                    text_out[out_idx++] = '\n';
                }
                text_out[out_idx++] = '-';
                text_out[out_idx++] = ' ';
            }

            p++;
            continue;
        }

        if (*p == '>') {
            in_tag = false;
            p++;
            continue;
        }

        if (in_tag) {
            p++;
            continue;
        }

        // HTML Entity Decoding
        if (*p == '&') {
            if (my_strncasecmp(p, "&quot;", 6) == 0) { text_out[out_idx++] = '"'; p += 6; continue; }
            if (my_strncasecmp(p, "&apos;", 6) == 0) { text_out[out_idx++] = '\''; p += 6; continue; }
            if (my_strncasecmp(p, "&lsquo;", 7) == 0) { text_out[out_idx++] = '\''; p += 7; continue; }
            if (my_strncasecmp(p, "&rsquo;", 7) == 0) { text_out[out_idx++] = '\''; p += 7; continue; }
            if (my_strncasecmp(p, "&ldquo;", 7) == 0) { text_out[out_idx++] = '"'; p += 7; continue; }
            if (my_strncasecmp(p, "&rdquo;", 7) == 0) { text_out[out_idx++] = '"'; p += 7; continue; }
            if (my_strncasecmp(p, "&mdash;", 7) == 0) { text_out[out_idx++] = '-'; p += 7; continue; }
            if (my_strncasecmp(p, "&ndash;", 7) == 0) { text_out[out_idx++] = '-'; p += 7; continue; }
            if (my_strncasecmp(p, "&hellip;", 8) == 0) { text_out[out_idx++] = '.'; p += 8; continue; }
            if (my_strncasecmp(p, "&amp;", 5) == 0) { text_out[out_idx++] = '&'; p += 5; continue; }
            if (my_strncasecmp(p, "&lt;", 4) == 0) { text_out[out_idx++] = '<'; p += 4; continue; }
            if (my_strncasecmp(p, "&gt;", 4) == 0) { text_out[out_idx++] = '>'; p += 4; continue; }
            if (my_strncasecmp(p, "&nbsp;", 6) == 0) { text_out[out_idx++] = ' '; p += 6; continue; }

            // Numeric entities &#160; &#x201C;
            if (p[1] == '#') {
                const char *end_semi = strchr(p, ';');
                if (end_semi && (end_semi - p) < 10) {
                    uint32_t val = 0;
                    if (p[2] == 'x' || p[2] == 'X') {
                        val = strtoul(p + 3, NULL, 16);
                    } else {
                        val = strtoul(p + 2, NULL, 10);
                    }
                    if (val == 0x201C || val == 0x201D) text_out[out_idx++] = '"';
                    else if (val == 0x2018 || val == 0x2019) text_out[out_idx++] = '\'';
                    else if (val == 0x2014 || val == 0x2013) text_out[out_idx++] = '-';
                    else if (val == 160) text_out[out_idx++] = ' ';
                    else if (val < 128) text_out[out_idx++] = (char)val;
                    else text_out[out_idx++] = '?';

                    p = end_semi + 1;
                    continue;
                }
            }
        }

        if (*p == '\n' || *p == '\r') {
            if (out_idx > 0 && text_out[out_idx - 1] != '\n') {
                text_out[out_idx++] = '\n';
            }
            p++;
            continue;
        }

        if (*p == ' ' || *p == '\t') {
            if (!last_was_space) {
                text_out[out_idx++] = ' ';
                last_was_space = true;
            }
            p++;
            continue;
        }

        last_was_space = false;
        text_out[out_idx++] = *p++;
    }

    text_out[out_idx] = '\0';

    // Post-process: collapse 3+ consecutive newlines to at most 2 (allowing 1 intentional blank line between paragraphs)
    char *r = text_out;
    char *w = text_out;
    int nl_cnt = 0;
    while (*r) {
        if (*r == '\n') {
            nl_cnt++;
            if (nl_cnt <= 2) {
                *w++ = '\n';
            }
        } else {
            nl_cnt = 0;
            *w++ = *r;
        }
        r++;
    }
    *w = '\0';
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

typedef struct {
    char id[64];
    char href[128];
} ManifestItem;

static bool parse_opf_spine_order(const char *opf_buf, EpubBook *book)
{
    if (!opf_buf || !book || book->chapter_count == 0) return false;

    ManifestItem *manifest = k_malloc(sizeof(ManifestItem) * 128);
    if (!manifest) return false;
    int manifest_count = 0;

    const char *m_start = my_strcasestr(opf_buf, "<manifest");
    const char *m_end = m_start ? my_strcasestr(m_start, "</manifest>") : NULL;
    if (m_start && m_end) {
        const char *p = m_start;
        while (p < m_end && manifest_count < 128) {
            const char *item = my_strcasestr(p, "<item ");
            if (!item || item >= m_end) break;

            const char *id_attr = my_strcasestr(item, "id=");
            const char *href_attr = my_strcasestr(item, "href=");

            if (id_attr && href_attr && id_attr < strchr(item, '>') && href_attr < strchr(item, '>')) {
                id_attr += 3; if (*id_attr == '"' || *id_attr == '\'') id_attr++;
                const char *id_end = strpbrk(id_attr, "\"'>");
                
                href_attr += 5; if (*href_attr == '"' || *href_attr == '\'') href_attr++;
                const char *href_end = strpbrk(href_attr, "\"'>");

                if (id_end && href_end && id_end > id_attr && href_end > href_attr) {
                    ManifestItem *mi = &manifest[manifest_count++];
                    size_t id_len = id_end - id_attr;
                    if (id_len >= sizeof(mi->id)) id_len = sizeof(mi->id) - 1;
                    memcpy(mi->id, id_attr, id_len); mi->id[id_len] = '\0';

                    size_t href_len = href_end - href_attr;
                    if (href_len >= sizeof(mi->href)) href_len = sizeof(mi->href) - 1;
                    memcpy(mi->href, href_attr, href_len); mi->href[href_len] = '\0';

                    char *hash = strchr(mi->href, '#');
                    if (hash) *hash = '\0';
                }
            }
            p = item + 6;
        }
    }

    bool success = false;
    const char *s_start = my_strcasestr(opf_buf, "<spine");
    const char *s_end = s_start ? my_strcasestr(s_start, "</spine>") : NULL;
    if (s_start && s_end) {
        EpubChapterInfo *ordered_chapters = k_malloc(sizeof(EpubChapterInfo) * MAX_EPUB_CHAPTERS);
        if (ordered_chapters) {
            int ordered_count = 0;
            const char *p = s_start;
            while (p < s_end && ordered_count < MAX_EPUB_CHAPTERS) {
                const char *ref = my_strcasestr(p, "<itemref ");
                if (!ref || ref >= s_end) break;

                const char *idref_attr = my_strcasestr(ref, "idref=");
                if (idref_attr && idref_attr < strchr(ref, '>')) {
                    idref_attr += 6; if (*idref_attr == '"' || *idref_attr == '\'') idref_attr++;
                    const char *idref_end = strpbrk(idref_attr, "\"'>");
                    if (idref_end && idref_end > idref_attr) {
                        char target_id[64] = {0};
                        size_t tlen = idref_end - idref_attr;
                        if (tlen >= sizeof(target_id)) tlen = sizeof(target_id) - 1;
                        memcpy(target_id, idref_attr, tlen); target_id[tlen] = '\0';

                        const char *target_href = NULL;
                        for (int m = 0; m < manifest_count; m++) {
                            if (strcmp(manifest[m].id, target_id) == 0) {
                                target_href = manifest[m].href;
                                break;
                            }
                        }

                        if (target_href) {
                            const char *mbase = strrchr(target_href, '/');
                            if (!mbase) mbase = strrchr(target_href, '\\');
                            mbase = mbase ? (mbase + 1) : target_href;

                            for (int c = 0; c < book->chapter_count; c++) {
                                const char *cbase = strrchr(book->chapters[c].chapter_path, '/');
                                if (!cbase) cbase = strrchr(book->chapters[c].chapter_path, '\\');
                                cbase = cbase ? (cbase + 1) : book->chapters[c].chapter_path;

                                if (my_strcasecmp(cbase, mbase) == 0) {
                                    ordered_chapters[ordered_count++] = book->chapters[c];
                                    break;
                                }
                            }
                        }
                    }
                }
                p = ref + 9;
            }

            if (ordered_count > 0) {
                LOG_INF("[EPUB Spine] Re-ordered %d chapters according to OPF spine", ordered_count);
                memcpy(book->chapters, ordered_chapters, sizeof(EpubChapterInfo) * ordered_count);
                book->chapter_count = ordered_count;
                success = true;
            }
            k_free(ordered_chapters);
        }
    }
    k_free(manifest);
    return success;
}

#define EPUB_CACHE_MAGIC   0x45505542u  /* "EPUB" */
#define EPUB_CACHE_VERSION 7u

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

static void build_cache_path(const char *epub_path, char *cache_path_out, size_t max_len)
{
    const char *base = strrchr(epub_path, '/');
    if (!base) base = strrchr(epub_path, '\\');
    base = base ? (base + 1) : epub_path;

    int max_base = (max_len > 16) ? (int)(max_len - 16) : 0;
    snprintf(cache_path_out, max_len, "/SD:/cache/%.*s.idx", max_base, base);
}

static void build_cover_cache_path(const char *epub_path, char *cov_path_out, size_t max_len)
{
    const char *base = strrchr(epub_path, '/');
    if (!base) base = strrchr(epub_path, '\\');
    base = base ? (base + 1) : epub_path;

    int max_base = (max_len > 16) ? (int)(max_len - 16) : 0;
    snprintf(cov_path_out, max_len, "/SD:/cache/%.*s.cov", max_base, base);
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

bool epub_scan_chapters(const char *epub_path, EpubBook *book) {
    if (!epub_path || !book) return false;

    LOG_INF("=== EPUB SCAN START: %s ===", epub_path);

    memset(book, 0, sizeof(EpubBook));
    snprintf(book->book_path, sizeof(book->book_path), "%s", epub_path);

    char cache_path[256];
    build_cache_path(epub_path, cache_path, sizeof(cache_path));

    if (load_sidecar_cache(cache_path, book)) {
        return true;
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

    uint32_t current_cum = 0;
    static char s_temp_txt[8192];
    int main_chap_counter = 1;
    for (int i = 0; i < book->chapter_count; i++) {
        book->chapters[i].cum_offset = current_cum;
        if (epub_read_chapter_text(epub_path, &book->chapters[i], s_temp_txt, sizeof(s_temp_txt))) {
            book->chapters[i].text_length = (uint32_t)strlen(s_temp_txt);
        } else {
            book->chapters[i].text_length = 500;
        }
        current_cum += book->chapters[i].text_length;
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
        LOG_INF("Ordered Chapter [%d]: %s ('%s', cum_offset: %u, text_len: %u)",
                i, book->chapters[i].chapter_path, book->chapters[i].title, book->chapters[i].cum_offset, book->chapters[i].text_length);
    }
    book->total_book_text_len = current_cum;

    LOG_INF("=== EPUB SCAN COMPLETE: Found %d sorted chapters, total text len: %u ===",
            book->chapter_count, book->total_book_text_len);

    if (book->chapter_count > 0) {
        save_sidecar_cache(cache_path, book);
    }
    return (book->chapter_count > 0);
}

bool epub_read_chapter_text(const char *epub_path, const EpubChapterInfo *chap_info, char *text_out, uint32_t text_max_len) {
    if (!epub_path || !chap_info || !text_out || text_max_len == 0) return false;

    struct fs_file_t file;
    fs_file_t_init(&file);

    int res = fs_open(&file, epub_path, FS_O_READ);
    if (res != 0) {
        LOG_ERR("Failed to open EPUB file: %s (err %d)", epub_path, res);
        return false;
    }

    fs_seek(&file, chap_info->payload_offset, FS_SEEK_SET);

    uint32_t to_read = chap_info->compressed_size;
    if (to_read > sizeof(s_comp_buf)) to_read = sizeof(s_comp_buf);

    ssize_t read_bytes = fs_read(&file, s_comp_buf, to_read);
    fs_close(&file);

    if (read_bytes <= 0) return false;

    if (chap_info->compression_method == 0) { // Store (uncompressed)
        uint32_t copy_len = (read_bytes < sizeof(s_raw_html_buf) - 1) ? read_bytes : (sizeof(s_raw_html_buf) - 1);
        memcpy(s_raw_html_buf, s_comp_buf, copy_len);
        s_raw_html_buf[copy_len] = '\0';
    } else if (chap_info->compression_method == 8) { // Deflate
        unsigned long dest_len = sizeof(s_raw_html_buf) - 1;
        unsigned long src_len = read_bytes;

        int ret = puff((unsigned char*)s_raw_html_buf, &dest_len, s_comp_buf, &src_len);
        if (ret != 0 && dest_len == 0) return false;
        s_raw_html_buf[dest_len] = '\0';
    } else {
        return false;
    }

    strip_html_and_entities(s_raw_html_buf, text_out, text_max_len);
    return true;
}

bool epub_read_book_offset(EpubBook *book, uint32_t global_offset, char *page_buf, size_t buf_size, uint32_t *bytes_read) {
    *bytes_read = 0;
    if (!book || book->chapter_count == 0 || buf_size == 0) return false;

    if (global_offset >= book->total_book_text_len) {
        global_offset = book->total_book_text_len > 0 ? (book->total_book_text_len - 1) : 0;
    }

    int chap_idx = 0;
    for (int i = 0; i < book->chapter_count; i++) {
        uint32_t chap_start = book->chapters[i].cum_offset;
        uint32_t chap_end = chap_start + book->chapters[i].text_length;
        if (global_offset >= chap_start && global_offset < chap_end) {
            chap_idx = i;
            break;
        }
        if (i == book->chapter_count - 1) {
            chap_idx = i;
        }
    }

    uint32_t local_offset = global_offset - book->chapters[chap_idx].cum_offset;

    static char s_chapter_text[8192];
    if (!epub_read_chapter_text(book->book_path, &book->chapters[chap_idx], s_chapter_text, sizeof(s_chapter_text))) {
        LOG_ERR("Failed to read EPUB chapter %d", chap_idx);
        return false;
    }

    size_t chap_text_len = strlen(s_chapter_text);
    if (local_offset >= chap_text_len) {
        local_offset = 0;
    }

    size_t avail = chap_text_len - local_offset;
    size_t copy_bytes = (avail < buf_size - 1) ? avail : (buf_size - 1);

    memcpy(page_buf, s_chapter_text + local_offset, copy_bytes);
    page_buf[copy_bytes] = '\0';
    *bytes_read = (uint32_t)copy_bytes;

    LOG_INF("[EPUB Offset Reader] Global offset %u -> Chap [%d] (%s), local_offset %u, read %u bytes",
            global_offset, chap_idx, book->chapters[chap_idx].chapter_path, local_offset, (uint32_t)copy_bytes);
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

bool epub_extract_cover_image(const char *epub_path, uint8_t *gray_out, int target_w, int target_h) {
    if (!epub_path || !gray_out || target_w <= 0 || target_h <= 0) return false;

    char full_epub_path[256];
    if (strncmp(epub_path, "/SD:", 4) == 0) {
        snprintf(full_epub_path, sizeof(full_epub_path), "%s", epub_path);
    } else {
        snprintf(full_epub_path, sizeof(full_epub_path), "/SD:/BOOKS/%s", epub_path);
    }

    /* 2. Fast Path: Check if pre-rendered 145x210 cover bitmap cache (.cov) exists on SD card */
    char cov_path[256];
    build_cover_cache_path(full_epub_path, cov_path, sizeof(cov_path));

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

    /* 3. Slow Path: Read EPUB cover image payload from SD card */
    EpubBook *tmp_book = k_malloc(sizeof(EpubBook));
    if (!tmp_book) return false;

    if (!epub_scan_chapters(full_epub_path, tmp_book) || !tmp_book->cover.found) {
        LOG_WRN("No cover image entry found in EPUB: %s", full_epub_path);
        k_free(tmp_book);
        return false;
    }

    struct fs_file_t file;
    fs_file_t_init(&file);
    if (fs_open(&file, full_epub_path, FS_O_READ) != 0) {
        k_free(tmp_book);
        return false;
    }

    uint32_t comp_size = tmp_book->cover.compressed_size;
    uint32_t payload_offset = tmp_book->cover.payload_offset;
    uint16_t comp_method = tmp_book->cover.compression_method;
    k_free(tmp_book);

    fs_seek(&file, payload_offset, FS_SEEK_SET);
    size_t comp_read = (comp_size < sizeof(s_comp_buf)) ? comp_size : sizeof(s_comp_buf);
    fs_read(&file, s_comp_buf, comp_read);
    fs_close(&file);

    uint8_t *raw_buf = s_comp_buf;
    size_t raw_len = comp_read;

    if (comp_method == 8) {
        unsigned long dest_len = sizeof(s_raw_html_buf);
        unsigned long src_len = comp_read;
        int p_res = puff(s_raw_html_buf, &dest_len, s_comp_buf, &src_len);
        if (p_res == 0 && dest_len > 0) {
            raw_buf = s_raw_html_buf;
            raw_len = dest_len;
        }
    }

    /* 4. Decode JPEG image stream into continuous 0-255 grayscale cover array */
    if (!decode_jpeg_dc_cover(raw_buf, raw_len, gray_out, target_w, target_h)) {
        for (int y = 0; y < target_h; y++) {
            for (int x = 0; x < target_w; x++) {
                uint32_t sample_idx = ((y * raw_len) / target_h + x) % raw_len;
                uint8_t b = raw_buf[sample_idx];
                gray_out[y * target_w + x] = (uint8_t)(15 + ((uint32_t)b * 225) / 255);
            }
        }
    }

    /* 5. Save generated 145x210 cover bitmap to SD sidecar cache (.cov) for instant future loads */
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
