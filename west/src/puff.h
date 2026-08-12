/* puff.h
 * Zero-heap, minimal RFC 1951 Deflate Inflation Algorithm by Mark Adler
 * Public Domain / Apache-2.0
 */

#ifndef PUFF_H
#define PUFF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * puff() decompresses a Deflate stream (RFC 1951).
 *
 *   dest:      pointer to destination buffer
 *   destlen:   pointer to destination length (in: capacity, out: decompressed size)
 *   source:    pointer to compressed source payload
 *   sourcelen: pointer to source length (in: source size, out: bytes consumed)
 *
 * Returns 0 on success, negative on error.
 *   0: success
 *   1: output buffer overflow
 *   2: input buffer ended prematurely
 *   3: distance too far back
 *  -1: invalid block type (3)
 *  -2: stored block length mismatch
 *  -3: dynamic block code length error
 *  -4: dynamic block code symbol error
 *  -5: dynamic block distance symbol error
 *  -6: dynamic block extra symbol error
 */
int puff(unsigned char *dest, unsigned long *destlen,
         const unsigned char *source, unsigned long *sourcelen);

#ifdef __cplusplus
}
#endif

#endif /* PUFF_H */
