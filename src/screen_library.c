// Library: books on the SD card
#include <string.h>
#include "book.h"
#include "config.h"
#include "display.h"
#include "screens.h"
#include "storage.h"

static BookList book_list;
static int selected = 0; // book_list.count selects "[Back]"

static void library_enter(SystemState from) {
    (void)from;
    storage_list_sd_books(&book_list);
    selected = 0;
    for (int i = 0; i < book_list.count; i++) {
        if (strcmp(book_list_name(&book_list, i), book_name()) == 0) {
            selected = i;
            break;
        }
    }
}

static void library_render(void) {
    const char *ptrs[MAX_BOOKS + 1];
    for (int i = 0; i < book_list.count; i++) ptrs[i] = book_list_label(&book_list, i);
    ptrs[book_list.count] = "[Back]";
    const char *footer = (book_list.count == 0) ? "No books found. Add .txt/.epub files to /BOOKS" : NULL;
    display_draw_menu_ext("Library", footer, ptrs, book_list.count + 1, selected);
}

static void library_step(int delta) {
    if (book_list.count > 0) {
        selected = wrap_index(selected, delta, book_list.count + 1);
        app_redraw();
    }
}

static void library_select(void) {
    if (selected < book_list.count) {
        book_open(book_list_name(&book_list, selected));
        app_go(STATE_READER);
    } else {
        app_go(STATE_MENU);
    }
}

const Screen screen_library = {
    .enter = library_enter,
    .render = library_render,
    .step = library_step,
    .select = library_select,
};
