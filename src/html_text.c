/* html_text.c
 * Streaming XHTML -> plain text converter. See html_text.h.
 */

#include "html_text.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>

enum { MODE_TEXT, MODE_TAG, MODE_ENTITY, MODE_COMMENT };

#define CODE_ITALIC    0x01
#define CODE_BOLD      0x02
#define CODE_UNDERLINE 0x04

static const char *const SKIP_ELEMENTS[] = { "head", "style", "script", "svg" };

static void flush(HtmlText *h) {
    if (h->olen == 0) return;
    if (!h->error && h->emit(h->ctx, h->obuf, h->olen) != 0) {
        h->error = 1;
    }
    h->olen = 0;
}

static void put_raw(HtmlText *h, char c) {
    if (h->olen == sizeof(h->obuf)) flush(h);
    h->obuf[h->olen++] = c;
    h->total++;
}

// Zero-width style codes don't affect spacing/newline bookkeeping
static void put_code(HtmlText *h, char code) {
    put_raw(h, code);
}

static void put_newline(HtmlText *h) {
    if (h->last == 0 || h->nl_run >= 2) return;
    put_raw(h, '\n');
    h->last = '\n';
    h->nl_run++;
    h->pending_space = false;
}

static void put_char(HtmlText *h, char c) {
    if (h->pending_space) {
        put_raw(h, ' ');
        h->pending_space = false;
    }
    put_raw(h, c);
    h->last = c;
    h->nl_run = 0;
}

static void put_str(HtmlText *h, const char *s) {
    while (*s) put_char(h, *s++);
}

static void line_break(HtmlText *h) {
    if (h->last != '\n') put_newline(h);
}

static void para_break(HtmlText *h) {
    put_newline(h);
    put_newline(h);
}

static void put_utf8(HtmlText *h, uint32_t cp) {
    if (cp < 0x80) {
        put_char(h, (char)cp);
    } else if (cp < 0x800) {
        put_char(h, (char)(0xC0 | (cp >> 6)));
        put_char(h, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        put_char(h, (char)(0xE0 | (cp >> 12)));
        put_char(h, (char)(0x80 | ((cp >> 6) & 0x3F)));
        put_char(h, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x110000) {
        put_char(h, (char)(0xF0 | (cp >> 18)));
        put_char(h, (char)(0x80 | ((cp >> 12) & 0x3F)));
        put_char(h, (char)(0x80 | ((cp >> 6) & 0x3F)));
        put_char(h, (char)(0x80 | (cp & 0x3F)));
    }
}

static void put_codepoint(HtmlText *h, uint32_t cp) {
    // Typography stays real UTF-8; the display falls back to ASCII per font
    if (cp == 160 || cp == 9 || cp == 10 || cp == 13 || cp == 32) {
        if (h->last != 0 && h->last != '\n') h->pending_space = true;
    }
    else if (cp >= 32) put_utf8(h, cp);
}

static void open_codes(HtmlText *h, uint8_t codes) {
    if (codes & CODE_ITALIC) put_code(h, '\x0E');
    if (codes & CODE_BOLD) put_code(h, '\x02');
    if (codes & CODE_UNDERLINE) put_code(h, '\x15');
}

static void close_codes(HtmlText *h, uint8_t codes) {
    if (codes & CODE_ITALIC) put_code(h, '\x0F');
    if (codes & CODE_BOLD) put_code(h, '\x03');
    if (codes & CODE_UNDERLINE) put_code(h, '\x16');
}

static bool name_is(const char *name, const char *a) {
    return strcmp(name, a) == 0;
}

static bool is_heading(const char *name) {
    return name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && name[2] == '\0';
}

// Inline CSS on the opening tag (lowercased attribute text)
static uint8_t style_codes_from_attrs(const char *attrs) {
    uint8_t codes = 0;
    if (strstr(attrs, "font-style: italic") || strstr(attrs, "font-style:italic") ||
        strstr(attrs, "font-style: oblique") || strstr(attrs, "font-style:oblique")) {
        codes |= CODE_ITALIC;
    }
    if (strstr(attrs, "font-weight: bold") || strstr(attrs, "font-weight:bold") ||
        strstr(attrs, "font-weight: 700") || strstr(attrs, "font-weight:700")) {
        codes |= CODE_BOLD;
    }
    if (strstr(attrs, "text-decoration: underline") || strstr(attrs, "text-decoration:underline")) {
        codes |= CODE_UNDERLINE;
    }
    return codes;
}

static void process_tag(HtmlText *h) {
    char *t = h->tag;
    t[h->tag_len] = '\0';

    if (t[0] == '!' || t[0] == '?') return; // doctype, xml declaration, CDATA

    for (char *c = t; *c; c++) *c = (char)tolower((unsigned char)*c);

    bool closing = (t[0] == '/');
    if (closing) t++;
    bool self_closing = (h->tag_len > 0 && h->tag[h->tag_len - 1] == '/');

    char name[12];
    size_t n = 0;
    while (*t && !isspace((unsigned char)*t) && *t != '/' && n < sizeof(name) - 1) {
        name[n++] = *t++;
    }
    name[n] = '\0';
    const char *attrs = t;

    if (h->skip[0]) {
        if (closing && name_is(name, h->skip)) h->skip[0] = '\0';
        return;
    }

    if (!closing && !self_closing) {
        for (size_t i = 0; i < sizeof(SKIP_ELEMENTS) / sizeof(SKIP_ELEMENTS[0]); i++) {
            if (name_is(name, SKIP_ELEMENTS[i])) {
                strcpy(h->skip, SKIP_ELEMENTS[i]);
                return;
            }
        }
    }

    // Block structure
    if (name_is(name, "p") || is_heading(name) || name_is(name, "blockquote")) {
        para_break(h);
    } else if (name_is(name, "br")) {
        put_newline(h);
    } else if (name_is(name, "div") || name_is(name, "tr") || name_is(name, "ul") || name_is(name, "ol")) {
        line_break(h);
    } else if (name_is(name, "li") && !closing) {
        line_break(h);
        put_str(h, "- ");
    }

    // Semantic inline styles
    if (name_is(name, "i") || name_is(name, "em") || name_is(name, "cite")) {
        put_code(h, closing ? '\x0F' : '\x0E');
    } else if (name_is(name, "b") || name_is(name, "strong") || is_heading(name)) {
        put_code(h, closing ? '\x03' : '\x02');
    } else if (name_is(name, "u") || name_is(name, "ins")) {
        put_code(h, closing ? '\x16' : '\x15');
    }

    // Inline CSS styles are closed again when their element closes
    if (self_closing) return;
    HtmlStyleFrame *top = h->style_depth ? &h->styles[h->style_depth - 1] : NULL;
    if (!closing) {
        uint8_t codes = style_codes_from_attrs(attrs);
        if (codes && h->style_depth < HTML_TEXT_STYLE_STACK) {
            HtmlStyleFrame *f = &h->styles[h->style_depth++];
            strcpy(f->name, name);
            f->nest = 0;
            f->codes = codes;
            open_codes(h, codes);
        } else if (top && name_is(name, top->name) && top->nest < 255) {
            top->nest++;
        }
    } else if (top && name_is(name, top->name)) {
        if (top->nest > 0) {
            top->nest--;
        } else {
            close_codes(h, top->codes);
            h->style_depth--;
        }
    }
}

static const struct {
    const char *name;
    uint32_t cp;
} NAMED_ENTITIES[] = {
    { "quot", '"' }, { "apos", '\'' }, { "amp", '&' }, { "lt", '<' }, { "gt", '>' },
    { "nbsp", 160 }, { "lsquo", 0x2018 }, { "rsquo", 0x2019 }, { "ldquo", 0x201C },
    { "rdquo", 0x201D }, { "mdash", 0x2014 }, { "ndash", 0x2013 }, { "hellip", 0x2026 },
};

static void process_entity(HtmlText *h) {
    h->ent[h->ent_len] = '\0';
    uint32_t cp = 0;

    if (h->ent[0] == '#') {
        if (h->ent[1] == 'x' || h->ent[1] == 'X') {
            cp = strtoul(h->ent + 2, NULL, 16);
        } else {
            cp = strtoul(h->ent + 1, NULL, 10);
        }
    } else {
        for (size_t i = 0; i < sizeof(NAMED_ENTITIES) / sizeof(NAMED_ENTITIES[0]); i++) {
            if (strcmp(h->ent, NAMED_ENTITIES[i].name) == 0) {
                cp = NAMED_ENTITIES[i].cp;
                break;
            }
        }
    }

    if (cp != 0) {
        put_codepoint(h, cp);
    } else {
        // Unknown entity: keep the source text
        put_char(h, '&');
        put_str(h, h->ent);
        put_char(h, ';');
    }
}

static void text_byte(HtmlText *h, uint8_t c) {
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        if (h->last != 0 && h->last != '\n') h->pending_space = true;
    } else if (c >= 32) {
        put_char(h, (char)c);
    }
}

void html_text_init(HtmlText *h, html_text_emit_fn emit, void *ctx) {
    memset(h, 0, sizeof(*h));
    h->emit = emit;
    h->ctx = ctx;
}

void html_text_feed(HtmlText *h, const uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        uint8_t c = buf[i];

        switch (h->mode) {
        case MODE_TAG:
            if (c == '>') {
                process_tag(h);
                h->mode = MODE_TEXT;
            } else if (h->tag_len < HTML_TEXT_TAG_MAX) {
                h->tag[h->tag_len++] = (char)c;
                if (h->tag_len == 3 && memcmp(h->tag, "!--", 3) == 0) {
                    h->mode = MODE_COMMENT;
                    h->comment_tail = 0;
                }
            }
            break;

        case MODE_COMMENT:
            if (c == '>' && h->comment_tail >= 2) {
                h->mode = MODE_TEXT;
            } else if (c == '-') {
                if (h->comment_tail < 2) h->comment_tail++;
            } else {
                h->comment_tail = 0;
            }
            break;

        case MODE_ENTITY:
            if (c == ';') {
                if (!h->skip[0]) process_entity(h);
                h->mode = MODE_TEXT;
            } else if (h->ent_len < sizeof(h->ent) - 1 && (isalnum(c) || c == '#')) {
                h->ent[h->ent_len++] = (char)c;
            } else {
                // Not an entity after all: emit the text and reprocess this byte
                if (!h->skip[0]) {
                    put_char(h, '&');
                    h->ent[h->ent_len] = '\0';
                    put_str(h, h->ent);
                }
                h->mode = MODE_TEXT;
                i--;
            }
            break;

        default:
            if (c == '<') {
                h->mode = MODE_TAG;
                h->tag_len = 0;
            } else if (c == '&') {
                h->mode = MODE_ENTITY;
                h->ent_len = 0;
            } else if (!h->skip[0]) {
                text_byte(h, c);
            }
            break;
        }
    }
}

void html_text_document_break(HtmlText *h) {
    // Close any inline styles left open so they can't bleed into the next document
    while (h->style_depth > 0) {
        close_codes(h, h->styles[--h->style_depth].codes);
    }
    para_break(h);
    h->mode = MODE_TEXT;
    h->skip[0] = '\0';
    h->pending_space = false;
}

int html_text_finish(HtmlText *h) {
    flush(h);
    return h->error;
}
