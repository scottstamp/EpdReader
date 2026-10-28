// To-do and grocery lists. Bluetooth sync runs while either lists screen is shown, with the
// reader's scratch buffer lent as the staging area for incoming documents.
#include <stdio.h>
#include "book.h"
#include "display.h"
#include "lists.h"
#include "lists_ble.h"
#include "screens.h"

#define LISTS_POLL_MS 300

static bool ble_active = false;
static bool shown_connected = false;
static int selected_list = 0;   // == list_count selects "[Back]"
static int open_list = 0;
static int selected_item = 0;   // == item_count: "Clear checked", +1: "[Back]"

static bool is_lists_screen(SystemState s) {
    return s == STATE_LISTS || s == STATE_LIST_ITEMS;
}

static void lists_screens_enter(SystemState from) {
    if (is_lists_screen(from)) return;
    display_set_portrait(true);  // tall lists: more items per screen
    lists_load();
    selected_list = 0;
    size_t size;
    uint8_t *buf = book_scratch(&size);
    ble_active = (lists_ble_start(buf, size) == 0);
}

static void lists_screens_leave(SystemState to) {
    if (is_lists_screen(to)) return;
    display_set_portrait(false);
    if (ble_active) {
        lists_ble_stop();
        ble_active = false;
    }
}

static const char *footer(void) {
    shown_connected = ble_active && lists_ble_connected();
    if (!ble_active) return "Bluetooth unavailable";
    return shown_connected ? "Browser connected" : "Bluetooth: ready to sync";
}

// After the lists were replaced from a browser, keep the selection on something that exists.
// Returns false if the open list is gone (and the overview was shown instead).
static bool clamp_selection(void) {
    const ListsDoc *doc = lists_get();
    if (selected_list > doc->list_count) selected_list = doc->list_count;
    if (app_state() == STATE_LIST_ITEMS) {
        if (open_list >= doc->list_count) {
            selected_list = doc->list_count ? doc->list_count - 1 : 0;
            app_go(STATE_LISTS);
            return false;
        }
        if (selected_item > doc->lists[open_list].item_count + 1) {
            selected_item = doc->lists[open_list].item_count + 1;
        }
    }
    return true;
}

static void lists_poll(void) {
    if (!ble_active) return;
    bool redraw = lists_ble_poll();
    if (redraw && !clamp_selection()) {
        return;
    }
    if (lists_ble_connected()) {
        app_keep_awake(); // no auto-sleep while a browser is linked
    }
    if (redraw || lists_ble_connected() != shown_connected) {
        app_redraw();
    }
}

// --- All lists ---

static void lists_render(void) {
    const ListsDoc *doc = lists_get();
    int count = doc->list_count;
    int first, visible;
    display_menu_window(count + 1, selected_list, &first, &visible);

    // Labels only for the rows on screen
    char labels[DISPLAY_MENU_ROWS][64];
    const char *options[LISTS_MAX_LISTS + 1];
    for (int i = 0; i <= count; i++) options[i] = "";
    for (int i = first; i < first + visible && i < count; i++) {
        const ListsList *l = &doc->lists[i];
        int done = 0;
        for (int k = 0; k < l->item_count; k++) done += doc->items[l->first_item + k].checked;
        snprintf(labels[i - first], sizeof(labels[0]), "%.*s  (%d/%d)", l->name_len,
                 doc->text + l->name_off, done, l->item_count);
        options[i] = labels[i - first];
    }
    options[count] = "[Back]";
    display_draw_menu_plain("Lists", footer(), options, count + 1, selected_list);
}

static void lists_step(int delta) {
    selected_list = wrap_index(selected_list, delta, lists_get()->list_count + 1);
    app_redraw();
}

static void lists_select(void) {
    if (selected_list < lists_get()->list_count) {
        open_list = selected_list;
        selected_item = 0;
        app_go(STATE_LIST_ITEMS);
    } else {
        app_go(STATE_MENU);
    }
}

const Screen screen_lists = {
    .enter = lists_screens_enter,
    .leave = lists_screens_leave,
    .render = lists_render,
    .step = lists_step,
    .select = lists_select,
    .poll = lists_poll,
    .poll_ms = LISTS_POLL_MS,
};

// --- One list ---

static void items_render(void) {
    const ListsDoc *doc = lists_get();
    const ListsList *l = &doc->lists[open_list];
    int count = l->item_count;
    int first, visible;
    display_menu_window(count + 2, selected_item, &first, &visible);

    char labels[DISPLAY_MENU_ROWS][LISTS_TEXT_MAX + 1];
    const char *options[LISTS_MAX_ITEMS + 2];
    int8_t checks[LISTS_MAX_ITEMS + 2];
    int done = 0;
    for (int i = 0; i < count; i++) {
        bool checked = doc->items[l->first_item + i].checked;
        done += checked;
        checks[i] = checked ? 1 : 0;
        options[i] = "";
    }
    for (int i = first; i < first + visible && i < count; i++) {
        const ListsItem *it = &doc->items[l->first_item + i];
        snprintf(labels[i - first], sizeof(labels[0]), "%.*s", it->text_len, doc->text + it->line_off + 4);
        options[i] = labels[i - first];
    }
    options[count] = "Clear checked";
    options[count + 1] = "[Back]";
    checks[count] = checks[count + 1] = -1;

    char title[LISTS_TEXT_MAX + 16];
    snprintf(title, sizeof(title), "%.*s (%d/%d)", l->name_len, doc->text + l->name_off, done, count);
    display_draw_checklist(title, footer(), options, checks, count + 2, selected_item);
}

static void items_step(int delta) {
    int rows = lists_get()->lists[open_list].item_count + 2;
    selected_item = wrap_index(selected_item, delta, rows);
    app_redraw();
}

static void items_select(void) {
    const ListsList *l = &lists_get()->lists[open_list];
    if (selected_item < l->item_count) {
        if (lists_toggle(l->first_item + selected_item) == LISTS_OK) {
            lists_ble_notify_changed();
        }
    } else if (selected_item == l->item_count) {
        if (lists_clear_checked(open_list) == LISTS_OK) {
            lists_ble_notify_changed();
        }
        selected_item = 0;
    } else {
        selected_list = open_list;
        app_go(STATE_LISTS);
        return;
    }
    app_redraw();
}

const Screen screen_list_items = {
    .enter = lists_screens_enter,
    .leave = lists_screens_leave,
    .render = items_render,
    .step = items_step,
    .select = items_select,
    .poll = lists_poll,
    .poll_ms = LISTS_POLL_MS,
};
