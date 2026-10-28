// Reader, its menu and the chapter list
#include <zephyr/kernel.h>
#include <stdio.h>
#include "book.h"
#include "buttons.h"
#include "display.h"
#include "screens.h"
#include "storage.h"

// --- Reader ---

static void reader_render(void) {
    reading_update(true);

    const EpubBook *epub = book_epub();
    char header[64] = "Reader";
    if (epub && epub->chapter_count > 0) {
        book_chapter_title(book_chapter_index(), header, sizeof(header));
    } else {
        storage_get_book_display_title(book_name(), header, sizeof(header));
    }

    if (!book_draw_page(header)) {
        display_draw_message(header, "Could not read book file!\nCheck SD card", true);
    }
}

// Leaving the reader banks the reading time and persists it with the stats
static void reader_leave(SystemState to) {
    (void)to;
    reading_update(false);
    book_flush_progress();
    user_settings_save();
}

static void reader_step(int delta) {
    if (delta > 0) {
        book_turn_forward(1 + buttons_take_repeats(BTN_EVENT_NEXT_CLICK));
    } else {
        book_turn_back(1 + buttons_take_repeats(BTN_EVENT_PREV_CLICK));
    }
    app_redraw();
}

static void reader_select(void) {
    app_go(STATE_READER_MENU);
}

const Screen screen_reader = {
    .leave = reader_leave,
    .render = reader_render,
    .step = reader_step,
    .select = reader_select,
};

// --- Reader menu ---

typedef enum {
    READER_MENU_RESUME,
    READER_MENU_CHAPTERS,
    READER_MENU_SETTINGS,
    READER_MENU_FULL_REFRESH,
    READER_MENU_SLEEP,
    READER_MENU_HOME,
    READER_MENU_COUNT
} ReaderMenuItem;

static int menu_selected = 0;

static void reader_menu_enter(SystemState from) {
    if (from == STATE_READER) {
        menu_selected = 0; // back from Settings keeps the position
    }
}

static void reader_menu_render(void) {
    char title[64];
    storage_get_book_display_title(book_name(), title, sizeof(title));

    char header[64];
    snprintf(header, sizeof(header), "%.48s (%u%%)", title[0] ? title : "Reader", book_progress_percent());

    uint32_t session_sec = reading_session_seconds();
    char session[32];
    snprintf(session, sizeof(session), "Session: %um %us", session_sec / 60, session_sec % 60);

    const char *options[READER_MENU_COUNT] = {
        [READER_MENU_RESUME] = "Resume reading",
        [READER_MENU_CHAPTERS] = "Chapters",
        [READER_MENU_SETTINGS] = "Settings",
        [READER_MENU_FULL_REFRESH] = "Clear ghosting (full refresh)",
        [READER_MENU_SLEEP] = "Sleep",
        [READER_MENU_HOME] = "Home",
    };
    display_draw_menu_ext(header, session, options, READER_MENU_COUNT, menu_selected);
}

static void reader_menu_step(int delta) {
    menu_selected = wrap_index(menu_selected, delta, READER_MENU_COUNT);
    app_redraw();
}

static void reader_menu_select(void) {
    switch ((ReaderMenuItem)menu_selected) {
        case READER_MENU_RESUME:
            app_go(STATE_READER);
            break;
        case READER_MENU_CHAPTERS:
            app_go(STATE_CHAPTER_LIST);
            break;
        case READER_MENU_SETTINGS:
            app_go(STATE_SETTINGS);
            break;
        case READER_MENU_FULL_REFRESH:
            display_request_full_refresh(); // applies to the page drawn next
            app_go(STATE_READER);
            break;
        case READER_MENU_SLEEP:
            app_sleep();
            break;
        case READER_MENU_HOME:
            app_go(STATE_MENU);
            break;
        default:
            break;
    }
}

const Screen screen_reader_menu = {
    .enter = reader_menu_enter,
    .render = reader_menu_render,
    .step = reader_menu_step,
    .select = reader_menu_select,
};

// --- Chapter list ---

static int chapter_selected = 0;

static int chapter_count(void) {
    const EpubBook *epub = book_epub();
    return epub ? MIN(epub->chapter_count, MAX_EPUB_CHAPTERS) : 0;
}

static void chapters_enter(SystemState from) {
    (void)from;
    chapter_selected = book_chapter_index();
}

static void chapters_render(void) {
    const EpubBook *epub = book_epub();
    const char *titles[MAX_EPUB_CHAPTERS];
    int count = chapter_count();
    for (int i = 0; i < count; i++) {
        titles[i] = epub->chapters[i].title;
    }
    display_draw_menu_ext("Select Chapter", NULL, titles, count, chapter_selected);
}

static void chapters_step(int delta) {
    int count = chapter_count();
    if (count > 0) {
        chapter_selected = wrap_index(chapter_selected, delta, count);
        app_redraw();
    }
}

static void chapters_select(void) {
    const EpubBook *epub = book_epub();
    if (epub && chapter_selected >= 0 && chapter_selected < chapter_count()) {
        book_jump(epub->chapters[chapter_selected].cum_offset);
    }
    app_go(STATE_READER);
}

const Screen screen_chapters = {
    .enter = chapters_enter,
    .render = chapters_render,
    .step = chapters_step,
    .select = chapters_select,
};
