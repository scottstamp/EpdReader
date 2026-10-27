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
struct fs_dirent { int type; size_t size; char name[256]; };
struct fs_file_t { int dummy; };
#define FS_O_READ 1
#define FS_SEEK_SET 0
static inline int fs_stat(const char *p, struct fs_dirent *e) { (void)p; (void)e; return -1; }
static inline void fs_file_t_init(struct fs_file_t *f) { (void)f; }
static inline int fs_open(struct fs_file_t *f, const char *p, int fl) { (void)f; (void)p; (void)fl; return -1; }
static inline int fs_close(struct fs_file_t *f) { (void)f; return 0; }
static inline ssize_t fs_read(struct fs_file_t *f, void *b, size_t n) { (void)f; (void)b; (void)n; return -1; }
static inline int fs_seek(struct fs_file_t *f, off_t o, int w) { (void)f; (void)o; (void)w; return -1; }
#define NRF_GPIO_PIN_MAP(p, n) (((p) << 5) | (n))
