// Minimal stand-ins for the Zephyr APIs display.c and font_extended.c use, so they
// compile on a PC for tools/ui_preview. Only what the renderer touches is provided.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <sys/types.h>
#define LOG_MODULE_REGISTER(...)
#define LOG_ERR(...)
#define LOG_WRN(...)
#define LOG_INF(...)
#define LOG_DBG(...)
#define BUILD_ASSERT(...)
#define ARG_UNUSED(x) (void)(x)
#ifndef MIN
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
#endif
#define BIT(n) (1u<<(n))
#ifndef CLAMP
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))
#endif
#define __aligned(x) __attribute__((aligned(x)))
static inline uint32_t k_uptime_get_32(void) { return 0; }
struct device { int dummy; };
#define DT_ALIAS(x) 0
#define DEVICE_DT_GET(x) (&stub_epd_device)
extern const struct device stub_epd_device;
static inline bool device_is_ready(const struct device *d) { (void)d; return true; }
struct display_buffer_descriptor { uint32_t buf_size; uint16_t width, height, pitch; };
int display_write(const struct device *dev, uint16_t x, uint16_t y, const struct display_buffer_descriptor *desc, const void *buf);
static inline int display_blanking_on(const struct device *d) { (void)d; return 0; }
// File system backed by stdio: "/SD:/x" maps to $UI_PREVIEW_SD_ROOT/x, so fonts load from the repo
#include <stdio.h>
#include <stdlib.h>
struct fs_dirent { int type; size_t size; char name[256]; };
struct fs_file_t { FILE *fp; };
#define FS_O_READ 1
#define FS_SEEK_SET SEEK_SET
static inline void stub_fs_path(const char *p, char *out, size_t n) {
    const char *root = getenv("UI_PREVIEW_SD_ROOT");
    if (strncmp(p, "/SD:", 4) == 0) snprintf(out, n, "%s%s", root ? root : ".", p + 4);
    else snprintf(out, n, "%s", p);
}
static inline int fs_stat(const char *p, struct fs_dirent *e) {
    char path[512];
    stub_fs_path(p, path, sizeof(path));
    FILE *fp = fopen(path, "rb");
    if (!fp) return -2;
    fseek(fp, 0, SEEK_END);
    e->type = 0;
    e->size = (size_t)ftell(fp);
    fclose(fp);
    return 0;
}
static inline void fs_file_t_init(struct fs_file_t *f) { f->fp = NULL; }
static inline int fs_open(struct fs_file_t *f, const char *p, int fl) {
    (void)fl;
    char path[512];
    stub_fs_path(p, path, sizeof(path));
    f->fp = fopen(path, "rb");
    return f->fp ? 0 : -2;
}
static inline int fs_close(struct fs_file_t *f) { if (f->fp) fclose(f->fp); f->fp = NULL; return 0; }
static inline ssize_t fs_read(struct fs_file_t *f, void *b, size_t n) { return f->fp ? (ssize_t)fread(b, 1, n, f->fp) : -5; }
static inline int fs_seek(struct fs_file_t *f, off_t o, int w) { return (f->fp && fseek(f->fp, (long)o, w) == 0) ? 0 : -5; }
#define NRF_GPIO_PIN_MAP(p, n) (((p) << 5) | (n))
