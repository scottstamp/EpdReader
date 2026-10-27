/* inflate_stream.h
 * Streaming RFC 1951 Deflate decoder derived from puff (Mark Adler).
 * Input is pulled through a read callback and output is pushed through a
 * write callback, so arbitrarily large entries can be decoded with a fixed
 * 32 KB history window.
 */

#ifndef INFLATE_STREAM_H
#define INFLATE_STREAM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define INFLATE_WINDOW_SIZE 32768u

/* Returns bytes placed in buf, 0 at end of input, negative on error. */
typedef int (*inflate_read_fn)(void *ctx, uint8_t *buf, size_t len);
/* Returns 0 on success, non-zero to abort decoding. */
typedef int (*inflate_write_fn)(void *ctx, const uint8_t *buf, size_t len);

/*
 * window: INFLATE_WINDOW_SIZE bytes of scratch history
 * inbuf:  input staging buffer of inbuf_size bytes
 *
 * Returns 0 on success, otherwise the same error codes as puff()
 * (2 = input ended early, negative = corrupt stream), or 4 when the write
 * callback aborted.
 */
int inflate_stream(uint8_t *window, uint8_t *inbuf, size_t inbuf_size,
                   inflate_read_fn rd, inflate_write_fn wr, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* INFLATE_STREAM_H */
