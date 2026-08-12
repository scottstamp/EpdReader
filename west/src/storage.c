#include "storage.h"
#include "epub.h"
#include <zephyr/fs/fs.h>
#include <zephyr/fs/fs_sys.h>
#include <ff.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

LOG_MODULE_REGISTER(storage, LOG_LEVEL_INF);

static FATFS fat_fs;
static struct fs_mount_t sd_mount_point = {
    .type = FS_FATFS,
    .fs_data = &fat_fs,
    .mnt_point = SD_MOUNT_POINT,
};

static EpubBook s_cached_epub;
static bool s_epub_cached = false;
static char s_cached_epub_path[256] = {0};

static bool is_valid_book_file(const char *str) {
    size_t len = strlen(str);
    if (len < 4) return false;

    // Check 4-char extension (.txt, .epu)
    const char *ext4 = str + len - 4;
    if (tolower((unsigned char)ext4[0]) == '.') {
        if (tolower((unsigned char)ext4[1]) == 't' &&
            tolower((unsigned char)ext4[2]) == 'x' &&
            tolower((unsigned char)ext4[3]) == 't') return true;
        if (tolower((unsigned char)ext4[1]) == 'e' &&
            tolower((unsigned char)ext4[2]) == 'p' &&
            tolower((unsigned char)ext4[3]) == 'u') return true;
    }

    // Check 5-char extension (.epub)
    if (len >= 5) {
        const char *ext5 = str + len - 5;
        if (tolower((unsigned char)ext5[0]) == '.' &&
            tolower((unsigned char)ext5[1]) == 'e' &&
            tolower((unsigned char)ext5[2]) == 'p' &&
            tolower((unsigned char)ext5[3]) == 'u' &&
            tolower((unsigned char)ext5[4]) == 'b') return true;
    }
    return false;
}

static bool ends_with_epub(const char *str) {
    size_t len = strlen(str);
    if (len < 4) return false;

    const char *ext4 = str + len - 4;
    if (tolower((unsigned char)ext4[0]) == '.' &&
        tolower((unsigned char)ext4[1]) == 'e' &&
        tolower((unsigned char)ext4[2]) == 'p' &&
        tolower((unsigned char)ext4[3]) == 'u') return true;

    if (len >= 5) {
        const char *ext5 = str + len - 5;
        if (tolower((unsigned char)ext5[0]) == '.' &&
            tolower((unsigned char)ext5[1]) == 'e' &&
            tolower((unsigned char)ext5[2]) == 'p' &&
            tolower((unsigned char)ext5[3]) == 'u' &&
            tolower((unsigned char)ext5[4]) == 'b') return true;
    }
    return false;
}

static void storage_dump_dir(const char *path, int depth) {
    struct fs_dir_t dir;
    struct fs_dirent entry;

    fs_dir_t_init(&dir);
    int res = fs_opendir(&dir, path);
    if (res != 0) {
        LOG_WRN("[SD TREE] Failed to open: %s (err=%d)", path, res);
        return;
    }

    while (fs_readdir(&dir, &entry) == 0 && entry.name[0] != '\0') {
        char full_path[256];
        if (path[strlen(path) - 1] == '/') {
            snprintf(full_path, sizeof(full_path), "%s%s", path, entry.name);
        } else {
            snprintf(full_path, sizeof(full_path), "%s/%s", path, entry.name);
        }

        if (entry.type == FS_DIR_ENTRY_DIR) {
            LOG_INF("[SD TREE] [DIR]  %s", full_path);
            if (depth < 3) {
                storage_dump_dir(full_path, depth + 1);
            }
        } else {
            LOG_INF("[SD TREE] [FILE] %s (%u bytes)", full_path, (uint32_t)entry.size);
        }
    }
    fs_closedir(&dir);
}

void storage_dump_tree(void) {
    LOG_INF("===================================");
    LOG_INF("=== SD CARD DIRECTORY STRUCTURE ===");
    storage_dump_dir(SD_MOUNT_POINT, 0);
    LOG_INF("===================================");
}

int storage_init(void) {
    int res = fs_mount(&sd_mount_point);
    if (res != 0) {
        LOG_WRN("SD card mount failed (res=%d). Running without SD card.", res);
        return res;
    }
    LOG_INF("SD card mounted successfully at %s", SD_MOUNT_POINT);

    storage_dump_tree();
    return 0;
}

int storage_list_sd_books(char books[][64], int max_books) {
    struct fs_dir_t dir;
    struct fs_dirent entry;
    int count = 0;

    const char *dirs_to_check[] = {
        "/SD:/BOOKS",
        "/SD:/books",
        "/SD:/Books",
        SD_MOUNT_POINT
    };

    for (int d = 0; d < 4 && count < max_books; d++) {
        fs_dir_t_init(&dir);
        int res = fs_opendir(&dir, dirs_to_check[d]);
        if (res != 0) {
            LOG_WRN("[SD List] Failed to open %s (err=%d)", dirs_to_check[d], res);
            continue;
        }

        while (count < max_books) {
            if (fs_readdir(&dir, &entry) != 0 || entry.name[0] == '\0') {
                break;
            }
            if (entry.type == FS_DIR_ENTRY_FILE && is_valid_book_file(entry.name)) {
                bool duplicate = false;
                for (int i = 0; i < count; i++) {
                    if (strcmp(books[i], entry.name) == 0) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate) {
                    snprintf(books[count], 64, "%s", entry.name);
                    LOG_INF("[SD Book Found] %s (in %s)", entry.name, dirs_to_check[d]);
                    count++;
                }
            }
        }
        fs_closedir(&dir);
    }

    return count;
}

static bool resolve_book_path(const char *book_name, char *out_path, size_t out_size) {
    if (strncmp(book_name, "/SD:", 4) == 0) {
        snprintf(out_path, out_size, "%s", book_name);
        return true;
    }

    const char *prefixes[] = {
        "/SD:/BOOKS",
        "/SD:/books",
        "/SD:/Books",
        "/SD:"
    };

    for (int i = 0; i < 4; i++) {
        snprintf(out_path, out_size, "%s/%s", prefixes[i], book_name);
        struct fs_dirent entry;
        if (fs_stat(out_path, &entry) == 0) {
            return true;
        }
    }
    snprintf(out_path, out_size, "/SD:/BOOKS/%s", book_name);
    return false;
}

uint32_t storage_get_book_size(const char *book_name) {
    char full_path[256];
    resolve_book_path(book_name, full_path, sizeof(full_path));

    if (ends_with_epub(book_name)) {
        if (s_epub_cached && strcmp(s_cached_epub_path, full_path) == 0) {
            return s_cached_epub.total_book_text_len;
        }
    }

    struct fs_dirent entry;
    if (fs_stat(full_path, &entry) == 0) {
        return (uint32_t)entry.size;
    }
    return 0;
}

bool storage_get_book_metadata(const char *book_name, char *title_out, size_t title_max, char *author_out, size_t author_max) {
    if (!book_name || book_name[0] == '\0') return false;

    if (title_out && title_max > 0) title_out[0] = '\0';
    if (author_out && author_max > 0) author_out[0] = '\0';

    char full_path[256];
    resolve_book_path(book_name, full_path, sizeof(full_path));

    if (ends_with_epub(book_name)) {
        if (!s_epub_cached || strcmp(s_cached_epub_path, full_path) != 0) {
            if (epub_scan_chapters(full_path, &s_cached_epub)) {
                snprintf(s_cached_epub_path, sizeof(s_cached_epub_path), "%s", full_path);
                s_epub_cached = true;
            }
        }
        if (s_epub_cached) {
            if (s_cached_epub.title[0] != '\0' && title_out) {
                snprintf(title_out, title_max, "%s", s_cached_epub.title);
            }
            if (s_cached_epub.author[0] != '\0' && author_out) {
                snprintf(author_out, author_max, "%s", s_cached_epub.author);
            }
        }
    }

    // Fallback: derive title from filename if empty
    if (title_out && title_out[0] == '\0') {
        const char *base = strrchr(book_name, '/');
        base = base ? base + 1 : book_name;
        snprintf(title_out, title_max, "%s", base);

        // Strip extension
        char *dot = strrchr(title_out, '.');
        if (dot) *dot = '\0';

        // Replace underscores/hyphens with spaces
        for (int i = 0; title_out[i]; i++) {
            if (title_out[i] == '_' || title_out[i] == '-') {
                title_out[i] = ' ';
            }
        }
    }
    return true;
}

bool storage_get_book_display_title(const char *book_name, char *out_str, size_t max_len) {
    if (!book_name || book_name[0] == '\0' || !out_str || max_len == 0) return false;

    char title[128] = {0};
    char author[128] = {0};
    storage_get_book_metadata(book_name, title, sizeof(title), author, sizeof(author));

    char formatted[256] = {0};
    if (author[0] != '\0') {
        snprintf(formatted, sizeof(formatted), "%s - %s", title, author);
    } else {
        snprintf(formatted, sizeof(formatted), "%s", title);
    }

    // Truncate to fit within top bar max length (e.g. 45 chars)
    if (strlen(formatted) > 45) {
        snprintf(out_str, max_len, "%.42s...", formatted);
    } else {
        snprintf(out_str, max_len, "%s", formatted);
    }
    return true;
}

bool storage_read_book_page(const char *book_name, uint32_t offset, char *page_buf, size_t buf_size, uint32_t *bytes_read) {
    *bytes_read = 0;
    if (!book_name || book_name[0] == '\0') return false;

    char full_path[256];
    resolve_book_path(book_name, full_path, sizeof(full_path));

    // Handle .epub files directly
    if (ends_with_epub(book_name)) {
        if (!s_epub_cached || strcmp(s_cached_epub_path, full_path) != 0) {
            if (!epub_scan_chapters(full_path, &s_cached_epub)) {
                LOG_ERR("Failed to scan EPUB chapters: %s", full_path);
                return false;
            }
            snprintf(s_cached_epub_path, sizeof(s_cached_epub_path), "%s", full_path);
            s_epub_cached = true;
        }

        return epub_read_book_offset(&s_cached_epub, offset, page_buf, buf_size, bytes_read);
    }

    // Standard .txt file reading
    struct fs_file_t file;
    fs_file_t_init(&file);
    int res = fs_open(&file, full_path, FS_O_READ);
    if (res != 0) {
        LOG_WRN("[SD Book] Failed to open %s (err=%d)", full_path, res);
        return false;
    }

    if (offset > 0) {
        fs_seek(&file, offset, FS_SEEK_SET);
    }

    int bytes = fs_read(&file, page_buf, buf_size - 1);
    fs_close(&file);

    if (bytes >= 0) {
        page_buf[bytes] = '\0';
        *bytes_read = (uint32_t)bytes;
        LOG_INF("[SD Book] Read %d bytes from %s at offset %u", bytes, full_path, offset);
        return true;
    }

    return false;
}

bool storage_read_progress(char *book_name, uint32_t *offset) {
    struct fs_dirent entry;
    if (fs_stat(PROGRESS_FILE, &entry) != 0) {
        return false;
    }

    struct fs_file_t file;
    fs_file_t_init(&file);
    if (fs_open(&file, PROGRESS_FILE, FS_O_READ) != 0) {
        return false;
    }

    char line[128] = {0};
    int bytes = fs_read(&file, line, sizeof(line) - 1);
    fs_close(&file);

    if (bytes <= 0) return false;
    line[bytes] = '\0';

    char *colon = strchr(line, ':');
    if (!colon) return false;

    *colon = '\0';
    snprintf(book_name, 64, "%s", line);
    *offset = (uint32_t)strtoul(colon + 1, NULL, 10);
    return true;
}

bool storage_write_progress(const char *book_name, uint32_t offset) {
    struct fs_file_t file;
    fs_file_t_init(&file);
    if (fs_open(&file, PROGRESS_FILE, FS_O_CREATE | FS_O_WRITE) != 0) {
        return false;
    }

    char line[128];
    int len = snprintf(line, sizeof(line), "%s:%u\n", book_name, offset);
    fs_write(&file, line, len);
    fs_close(&file);
    return true;
}

bool storage_read_stats(uint32_t *out_total_seconds) {
    *out_total_seconds = 0;
    struct fs_dirent entry;
    if (fs_stat(STATS_FILE, &entry) != 0) {
        return false;
    }

    struct fs_file_t file;
    fs_file_t_init(&file);
    if (fs_open(&file, STATS_FILE, FS_O_READ) != 0) {
        return false;
    }

    char line[32] = {0};
    int bytes = fs_read(&file, line, sizeof(line) - 1);
    fs_close(&file);

    if (bytes <= 0) return false;
    line[bytes] = '\0';

    *out_total_seconds = (uint32_t)strtoul(line, NULL, 10);
    return true;
}

bool storage_write_stats(uint32_t total_seconds) {
    struct fs_file_t file;
    fs_file_t_init(&file);
    if (fs_open(&file, STATS_FILE, FS_O_CREATE | FS_O_WRITE) != 0) {
        return false;
    }

    char line[32];
    int len = snprintf(line, sizeof(line), "%u\n", total_seconds);
    fs_write(&file, line, len);
    fs_close(&file);
    return true;
}
