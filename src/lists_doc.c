#include "lists_doc.h"

#include <stdio.h>
#include <string.h>

static const char DEFAULT_DOC[] =
    "@rev 0\n"
    "# Groceries\n"
    "# To-do\n";

static bool text_ok(const char *s, size_t len) {
    if (len == 0 || len > LISTS_TEXT_MAX) return false;
    for (size_t i = 0; i < len; i++) {
        // UTF-8 bytes (>= 0x80) are fine; ASCII control characters are not
        if ((uint8_t)s[i] < 0x20 || s[i] == 0x7F) return false;
    }
    return true;
}

static bool parse_rev(const char *line, size_t len, uint32_t *rev) {
    if (len < 6 || memcmp(line, "@rev ", 5) != 0) return false;
    uint32_t v = 0;
    for (size_t i = 5; i < len; i++) {
        if (line[i] < '0' || line[i] > '9') return false;
        if (v > (UINT32_MAX - 9) / 10) return false;
        v = v * 10 + (uint32_t)(line[i] - '0');
    }
    *rev = v;
    return true;
}

// Shared by parse and validate; doc may be NULL to only check the text
static int parse_text(const char *text, size_t len, ListsDoc *doc, uint32_t *rev_out) {
    if (len > LISTS_DOC_MAX) return LISTS_ERR_TOO_BIG;

    size_t pos = 0;
    int lists = 0, items = 0;
    bool have_rev = false;

    while (pos < len) {
        const char *line = text + pos;
        const char *nl = memchr(line, '\n', len - pos);
        size_t line_len = nl ? (size_t)(nl - line) : len - pos;
        size_t next = pos + line_len + (nl ? 1 : 0);

        if (!have_rev) {
            uint32_t rev;
            if (!parse_rev(line, line_len, &rev)) return LISTS_ERR_NO_REV;
            if (rev_out) *rev_out = rev;
            have_rev = true;
        } else if (line_len == 0) {
            // blank line
        } else if (line_len >= 2 && line[0] == '#' && line[1] == ' ') {
            if (!text_ok(line + 2, line_len - 2)) return LISTS_ERR_TEXT;
            if (lists == LISTS_MAX_LISTS) return LISTS_ERR_TOO_BIG;
            if (doc) {
                ListsList *l = &doc->lists[lists];
                l->name_off = (uint16_t)(pos + 2);
                l->name_len = (uint8_t)(line_len - 2);
                l->item_count = 0;
                l->first_item = (uint16_t)items;
            }
            lists++;
        } else if (line_len >= 4 && line[0] == '[' && (line[1] == ' ' || line[1] == 'x') &&
                   line[2] == ']' && line[3] == ' ') {
            if (lists == 0) return LISTS_ERR_ORPHAN_ITEM;
            if (!text_ok(line + 4, line_len - 4)) return LISTS_ERR_TEXT;
            if (items == LISTS_MAX_ITEMS) return LISTS_ERR_TOO_BIG;
            if (doc) {
                ListsItem *it = &doc->items[items];
                it->line_off = (uint16_t)pos;
                it->text_len = (uint8_t)(line_len - 4);
                it->checked = (line[1] == 'x');
                doc->lists[lists - 1].item_count++;
            }
            items++;
        } else {
            return LISTS_ERR_BAD_LINE;
        }
        pos = next;
    }
    if (!have_rev) return LISTS_ERR_NO_REV;

    if (doc) {
        doc->list_count = (uint8_t)lists;
        doc->item_count = (uint16_t)items;
    }
    return LISTS_OK;
}

int lists_doc_validate(const char *text, size_t len, uint32_t *rev_out) {
    return parse_text(text, len, NULL, rev_out);
}

int lists_doc_parse(ListsDoc *doc) {
    doc->text[doc->len] = '\0';
    int err = parse_text(doc->text, doc->len, doc, &doc->rev);
    if (err) {
        doc->list_count = 0;
        doc->item_count = 0;
    }
    return err;
}

void lists_doc_set_default(ListsDoc *doc) {
    size_t n = sizeof(DEFAULT_DOC) - 1;
    memcpy(doc->text, DEFAULT_DOC, n);
    doc->len = (uint16_t)n;
    lists_doc_parse(doc);
}

// Replaces text[off, off+old_len) with repl
static int splice(ListsDoc *doc, size_t off, size_t old_len, const char *repl, size_t new_len) {
    if (doc->len - old_len + new_len > doc->cap) return LISTS_ERR_TOO_BIG;
    memmove(doc->text + off + new_len, doc->text + off + old_len, doc->len - off - old_len);
    memcpy(doc->text + off, repl, new_len);
    doc->len = (uint16_t)(doc->len - old_len + new_len);
    doc->text[doc->len] = '\0';
    return LISTS_OK;
}

int lists_doc_set_rev(ListsDoc *doc, uint32_t rev) {
    const char *nl = memchr(doc->text, '\n', doc->len);
    size_t old_len = nl ? (size_t)(nl - doc->text) : doc->len;
    char line[16];
    int n = snprintf(line, sizeof(line), "@rev %u", (unsigned)rev);
    int err = splice(doc, 0, old_len, line, (size_t)n);
    if (err) return err;
    return lists_doc_parse(doc);
}

int lists_doc_toggle(ListsDoc *doc, int item_index) {
    if (item_index < 0 || item_index >= doc->item_count) return LISTS_ERR_BAD_LINE;
    char *mark = doc->text + doc->items[item_index].line_off + 1;
    *mark = (*mark == 'x') ? ' ' : 'x';
    return lists_doc_set_rev(doc, doc->rev + 1);
}

int lists_doc_clear_checked(ListsDoc *doc, int list_index) {
    if (list_index < 0 || list_index >= doc->list_count) return LISTS_ERR_BAD_LINE;
    const ListsList *l = &doc->lists[list_index];
    bool changed = false;
    // Last to first, so earlier offsets stay valid while lines are removed
    for (int i = l->first_item + l->item_count - 1; i >= l->first_item; i--) {
        const ListsItem *it = &doc->items[i];
        if (!it->checked) continue;
        size_t line_len = 4u + it->text_len;
        if (it->line_off + line_len < doc->len) line_len++;  // its newline
        splice(doc, it->line_off, line_len, "", 0);
        changed = true;
    }
    if (!changed) return LISTS_OK;
    return lists_doc_set_rev(doc, doc->rev + 1);
}
