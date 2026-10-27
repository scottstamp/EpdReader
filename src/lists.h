#ifndef LISTS_H
#define LISTS_H

#include "lists_doc.h"

// The device's lists, kept in RAM and saved to /SD:/lists.txt. Main thread only.

// Error codes for lists_replace beyond enum lists_err
#define LISTS_ERR_CONFLICT 100  // client edited an older revision
#define LISTS_ERR_STORAGE  101

void lists_load(void);
const ListsDoc *lists_get(void);

// Device-side edits; saved immediately. Return LISTS_OK or an error.
int lists_toggle(int item_index);
int lists_clear_checked(int list_index);

// A client's edited copy, made from revision base_rev. Accepted only if nothing changed
// on the device since; the new revision is base_rev + 1.
int lists_replace(const char *text, size_t len, uint32_t base_rev);

#endif // LISTS_H
