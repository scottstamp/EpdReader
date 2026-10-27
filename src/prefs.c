#include "prefs.h"
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>
#include <stddef.h>
#include <string.h>

LOG_MODULE_REGISTER(prefs, LOG_LEVEL_INF);

#define PREFS_SUBTREE "epd"
#define PREFS_NAME    "prefs1"

static Prefs s_stored;
static bool s_found;

// Size written by firmware before frame_rate was added
#define PREFS_V1_SIZE offsetof(Prefs, frame_rate)
BUILD_ASSERT(PREFS_V1_SIZE == 12, "the old Prefs layout must stay a prefix");

static int prefs_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    if (strcmp(name, PREFS_NAME) == 0 && (len == sizeof(Prefs) || len == PREFS_V1_SIZE)) {
        memset(&s_stored, 0, sizeof(s_stored));
        if (read_cb(cb_arg, &s_stored, len) == (ssize_t)len) {
            s_found = true;
        }
    }
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(epd_prefs, PREFS_SUBTREE, NULL, prefs_set, NULL, NULL);

bool prefs_load(Prefs *out) {
    int err = settings_subsys_init();
    if (err) {
        LOG_ERR("Settings init failed (err %d)", err);
        return false;
    }
    settings_load_subtree(PREFS_SUBTREE);
    if (s_found) {
        *out = s_stored;
    }
    return s_found;
}

int prefs_save(const Prefs *prefs) {
    if (s_found && memcmp(prefs, &s_stored, sizeof(Prefs)) == 0) {
        return 0;
    }
    int err = settings_save_one(PREFS_SUBTREE "/" PREFS_NAME, prefs, sizeof(Prefs));
    if (err) {
        LOG_ERR("Saving preferences failed (err %d)", err);
        return err;
    }
    s_stored = *prefs;
    s_found = true;
    return 0;
}
