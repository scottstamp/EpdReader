#ifndef LISTS_DOC_H
#define LISTS_DOC_H

// To-do / grocery lists document. Pure C (no Zephyr) so it can be tested on the host.
//
// The document is UTF-8 text, kept and transferred verbatim:
//
//   @rev 12
//   # Groceries
//   [ ] Milk
//   [x] Eggs
//   # To-do
//   [ ] Call the bank
//
// - The first line is "@rev <n>": bumped on every change, used to detect edits
//   made on the device while a client was editing an older copy.
// - "# <name>" starts a list; "[ ] <text>" / "[x] <text>" are its items.
// - Lines end with "\n"; empty lines are allowed and ignored. Anything else is
//   rejected, so a client bug can't silently corrupt the lists.
//
// The parsed index points into the text, so edits on the device (check an item,
// clear checked items) are done in place on the text and the index rebuilt.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LISTS_DOC_MAX   3072   // bytes of text
// Largest document a client may send: leaves room for the device to lengthen "@rev"
#define LISTS_PUT_MAX   (LISTS_DOC_MAX - 12)
#define LISTS_MAX_LISTS 16
#define LISTS_MAX_ITEMS 128    // across all lists
#define LISTS_TEXT_MAX  96     // bytes in a list name or item text

enum lists_err {
    LISTS_OK = 0,
    LISTS_ERR_TOO_BIG,      // over LISTS_DOC_MAX, or too many lists/items
    LISTS_ERR_NO_REV,       // first line isn't "@rev <n>"
    LISTS_ERR_BAD_LINE,     // a line that is neither a header nor an item
    LISTS_ERR_ORPHAN_ITEM,  // item before the first list header
    LISTS_ERR_TEXT,         // empty or over-long name/text, or a control character
};

typedef struct {
    uint16_t name_off;     // into text
    uint8_t name_len;
    uint8_t item_count;
    uint16_t first_item;   // index into items[]
} ListsList;

typedef struct {
    uint16_t line_off;     // the '[' of "[ ] text"
    uint8_t text_len;      // text starts at line_off + 4
    bool checked;
} ListsItem;

typedef struct {
    char *text;            // NUL-terminated, capacity cap + 1
    uint16_t len;
    uint16_t cap;
    uint32_t rev;
    uint8_t list_count;
    uint16_t item_count;
    ListsList lists[LISTS_MAX_LISTS];
    ListsItem items[LISTS_MAX_ITEMS];
} ListsDoc;

// Parses the text (doc->text, doc->len) and builds the index. On error the index is empty.
int lists_doc_parse(ListsDoc *doc);
// Validates text without touching any document
int lists_doc_validate(const char *text, size_t len, uint32_t *rev_out);

// Replaces the content with the built-in starter lists (rev 0)
void lists_doc_set_default(ListsDoc *doc);

// Device-side edits; each bumps rev. Return LISTS_OK or an error code.
int lists_doc_toggle(ListsDoc *doc, int item_index);
int lists_doc_clear_checked(ListsDoc *doc, int list_index);
// Rewrites the "@rev" line (used when a client's copy is accepted)
int lists_doc_set_rev(ListsDoc *doc, uint32_t rev);

#endif // LISTS_DOC_H
