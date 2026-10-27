// Host tests for src/lists_doc.c.
//   doc_test              run the unit tests
//   doc_test --check      read a document on stdin, print "ok <rev> <lists> <items> <checked>"
//                         or "err <code>" (used by test_web.mjs to check the web page's encoder)
//
// Build: gcc -Wall -Wextra -Werror -std=c11 -I src tools/lists_web/test/doc_test.c src/lists_doc.c

#include "lists_doc.h"

#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static char buf[LISTS_DOC_MAX + 1];
static ListsDoc doc = { .text = buf, .cap = LISTS_DOC_MAX };

static int load(const char *text) {
    doc.len = (uint16_t)strlen(text);
    memcpy(buf, text, doc.len);
    return lists_doc_parse(&doc);
}

static int checked_count(void) {
    int n = 0;
    for (int i = 0; i < doc.item_count; i++) n += doc.items[i].checked;
    return n;
}

static void test_parse(void) {
    CHECK(load("@rev 3\n# Groceries\n[ ] Milk\n[x] Eggs\n\n# To-do\n[ ] Call the bank\n") == LISTS_OK);
    CHECK(doc.rev == 3 && doc.list_count == 2 && doc.item_count == 3);
    CHECK(doc.lists[0].item_count == 2 && doc.lists[1].first_item == 2 && doc.lists[1].item_count == 1);
    CHECK(doc.items[1].checked && !doc.items[0].checked);
    CHECK(doc.items[2].text_len == strlen("Call the bank"));
    CHECK(memcmp(doc.text + doc.lists[1].name_off, "To-do", 5) == 0);

    CHECK(load("@rev 0") == LISTS_OK && doc.list_count == 0);          // no trailing newline
    CHECK(load("@rev 7\n# Empty list\n") == LISTS_OK && doc.lists[0].item_count == 0);
    CHECK(load("@rev 1\n# Caf\xc3\xa9\n[ ] cr\xc3\xa8me br\xc3\xbbl\xc3\xa9" "e\n") == LISTS_OK);  // UTF-8

    CHECK(load("") == LISTS_ERR_NO_REV);
    CHECK(load("# Groceries\n") == LISTS_ERR_NO_REV);
    CHECK(load("@rev x\n") == LISTS_ERR_NO_REV);
    CHECK(load("@rev 99999999999\n") == LISTS_ERR_NO_REV);            // over 32 bits
    CHECK(load("@rev 1\n[ ] orphan\n") == LISTS_ERR_ORPHAN_ITEM);
    CHECK(load("@rev 1\n# L\nMilk\n") == LISTS_ERR_BAD_LINE);
    CHECK(load("@rev 1\n# L\n[X] Milk\n") == LISTS_ERR_BAD_LINE);
    CHECK(load("@rev 1\n# L\n[ ]Milk\n") == LISTS_ERR_BAD_LINE);
    CHECK(load("@rev 1\n# L\r\n") == LISTS_ERR_TEXT);                 // CR is a control character
    CHECK(load("@rev 1\n# \n") == LISTS_ERR_TEXT);
    CHECK(load("@rev 1\n# L\n[ ] \n") == LISTS_ERR_TEXT);
    CHECK(lists_doc_parse(&doc) != LISTS_OK || doc.list_count == 0);  // failed parse leaves an empty index

    char big[400];
    strcpy(big, "@rev 1\n# ");
    memset(big + 9, 'a', LISTS_TEXT_MAX);
    big[9 + LISTS_TEXT_MAX] = '\0';
    CHECK(load(big) == LISTS_OK);
    strcat(big, "a");
    CHECK(load(big) == LISTS_ERR_TEXT);

    // Limits on lists and items
    static char many[LISTS_DOC_MAX + 64];
    strcpy(many, "@rev 1\n");
    for (int i = 0; i <= LISTS_MAX_LISTS; i++) strcat(many, "# L\n");
    CHECK(lists_doc_validate(many, strlen(many), NULL) == LISTS_ERR_TOO_BIG);
    strcpy(many, "@rev 1\n# L\n");
    for (int i = 0; i <= LISTS_MAX_ITEMS; i++) strcat(many, "[ ] i\n");
    CHECK(lists_doc_validate(many, strlen(many), NULL) == LISTS_ERR_TOO_BIG);
    CHECK(lists_doc_validate(many, LISTS_DOC_MAX + 1, NULL) == LISTS_ERR_TOO_BIG);
}

static void test_edits(void) {
    CHECK(load("@rev 9\n# G\n[ ] Milk\n[x] Eggs\n[ ] Tea\n# T\n[x] Bank\n") == LISTS_OK);
    CHECK(lists_doc_toggle(&doc, 0) == LISTS_OK);
    CHECK(doc.rev == 10 && doc.items[0].checked);                     // "@rev 9" -> "@rev 10" grew a byte
    CHECK(strncmp(doc.text, "@rev 10\n# G\n[x] Milk\n", 21) == 0);
    CHECK(lists_doc_toggle(&doc, 0) == LISTS_OK && !doc.items[0].checked && doc.rev == 11);
    CHECK(lists_doc_toggle(&doc, 99) != LISTS_OK && doc.rev == 11);

    CHECK(lists_doc_clear_checked(&doc, 0) == LISTS_OK);
    CHECK(doc.rev == 12 && doc.item_count == 3 && doc.lists[0].item_count == 2);
    CHECK(strcmp(doc.text, "@rev 12\n# G\n[ ] Milk\n[ ] Tea\n# T\n[x] Bank\n") == 0);
    CHECK(lists_doc_clear_checked(&doc, 0) == LISTS_OK && doc.rev == 12);  // nothing to clear: no change
    CHECK(lists_doc_clear_checked(&doc, 1) == LISTS_OK);
    CHECK(strcmp(doc.text, "@rev 13\n# G\n[ ] Milk\n[ ] Tea\n# T\n") == 0 && doc.lists[1].item_count == 0);

    // Last line without a newline
    CHECK(load("@rev 1\n# G\n[ ] a\n[x] b") == LISTS_OK);
    CHECK(lists_doc_clear_checked(&doc, 0) == LISTS_OK);
    CHECK(strcmp(doc.text, "@rev 2\n# G\n[ ] a\n") == 0);

    CHECK(lists_doc_set_rev(&doc, 4000000000u) == LISTS_OK && doc.rev == 4000000000u);
    CHECK(checked_count() == 0);

    lists_doc_set_default(&doc);
    CHECK(doc.rev == 0 && doc.list_count == 2 && doc.item_count == 0);
}

static int check_stdin(void) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);  // text mode would turn CR LF into LF
#endif
    size_t n = fread(buf, 1, LISTS_DOC_MAX + 1, stdin);
    if (n > LISTS_DOC_MAX) {
        printf("err %d\n", LISTS_ERR_TOO_BIG);
        return 0;
    }
    doc.len = (uint16_t)n;
    int err = lists_doc_parse(&doc);
    if (err) {
        printf("err %d\n", err);
    } else {
        printf("ok %u %u %u %d\n", (unsigned)doc.rev, doc.list_count, doc.item_count, checked_count());
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--check") == 0) {
        return check_stdin();
    }
    test_parse();
    test_edits();
    printf(failures ? "%d FAILURES\n" : "all lists_doc tests passed\n", failures);
    return failures ? 1 : 0;
}
