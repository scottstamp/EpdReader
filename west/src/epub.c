#include "epub.h"
#include "puff.h"
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

LOG_MODULE_REGISTER(epub, LOG_LEVEL_INF);

static uint8_t s_comp_buf[16384];   // 16 KB static compressed payload buffer
static char s_raw_html_buf[32768];  // 32 KB static inflated HTML buffer

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
            // Format headings and paragraphs cleanly
            if (my_strncasecmp(p, "<p>", 3) == 0 || my_strncasecmp(p, "<p ", 3) == 0) {
                if (out_idx > 0 && text_out[out_idx - 1] != '\n') {
                    text_out[out_idx++] = '\n';
                }
                text_out[out_idx++] = ' ';
                text_out[out_idx++] = ' ';
            } else if (my_strncasecmp(p, "<br", 3) == 0 || my_strncasecmp(p, "</div>", 6) == 0) {
                if (out_idx > 0 && text_out[out_idx - 1] != '\n') {
                    text_out[out_idx++] = '\n';
                }
            } else if (p[1] == 'h' || p[1] == 'H') {
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

    // Post-process: collapse any multiple consecutive newlines to strictly 1 single newline
    char *r = text_out;
    char *w = text_out;
    int nl_cnt = 0;
    while (*r) {
        if (*r == '\n') {
            nl_cnt++;
            if (nl_cnt <= 1) {
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

bool epub_scan_chapters(const char *epub_path, EpubBook *book) {
    if (!epub_path || !book) return false;

    LOG_INF("=== EPUB SCAN START: %s ===", epub_path);

    memset(book, 0, sizeof(EpubBook));
    snprintf(book->book_path, sizeof(book->book_path), "%s", epub_path);

    struct fs_file_t file;
    fs_file_t_init(&file);

    int res = fs_open(&file, epub_path, FS_O_READ);
    if (res != 0) {
        LOG_ERR("Failed to open EPUB file: %s (err %d)", epub_path, res);
        return false;
    }

    uint32_t curr_pos = 0;
    int scanned_headers = 0;

    while (book->chapter_count < MAX_EPUB_CHAPTERS && scanned_headers < 200) {
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

        bool is_html = (strstr(lower_name, ".xhtml") != NULL || strstr(lower_name, ".html") != NULL || strstr(lower_name, ".htm") != NULL);
        bool is_nav = (strstr(lower_name, "container.xml") != NULL || strstr(lower_name, "content.opf") != NULL ||
                        strstr(lower_name, "toc.ncx") != NULL || strstr(lower_name, "nav.xhtml") != NULL ||
                        strstr(lower_name, "cover") != NULL || strstr(lower_name, "next-reads") != NULL);

        if ((strstr(lower_name, "content.opf") != NULL || strstr(lower_name, ".opf") != NULL) && uncomp_size > 0 && uncomp_size < 32768) {
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

    if (book->chapter_count > 1) {
        qsort(book->chapters, book->chapter_count, sizeof(EpubChapterInfo), natural_chapter_cmp);
    }

    uint32_t current_cum = 0;
    static char s_temp_txt[8192];
    for (int i = 0; i < book->chapter_count; i++) {
        book->chapters[i].cum_offset = current_cum;
        if (epub_read_chapter_text(epub_path, &book->chapters[i], s_temp_txt, sizeof(s_temp_txt))) {
            book->chapters[i].text_length = (uint32_t)strlen(s_temp_txt);
        } else {
            book->chapters[i].text_length = 500;
        }
        current_cum += book->chapters[i].text_length;
        LOG_INF("Ordered Chapter [%d]: %s (cum_offset: %u, text_len: %u)",
                i, book->chapters[i].chapter_path, book->chapters[i].cum_offset, book->chapters[i].text_length);
    }
    book->total_book_text_len = current_cum;

    LOG_INF("=== EPUB SCAN COMPLETE: Found %d sorted chapters, total text len: %u ===",
            book->chapter_count, book->total_book_text_len);
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
