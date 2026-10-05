#ifndef ACPCM_H
#define ACPCM_H

/* acpcm - sample-domain DPCM coder with an adaptive binary range coder.
 *
 * A completely different scheme from the variable-degree polynomial codec in
 * nofft.cpp: this one keeps no polynomial at all.  Each frame of N samples is
 * coded as a first difference against the previous reconstructed sample, the
 * difference is quantised to a uniform step, and the resulting symbols go
 * through an LZMA-style carryless range coder.
 *
 * Container layout, all integers little endian:
 *
 *   0   char     magic[4]   "NFA1"
 *   4   uint32   frames     frames per channel
 *   8   uint32   bits       quantiser width, 2..24
 *   12  uint32   channels
 *   16  uint32   sample_rate
 *   20  uint32   len[c]     byte length of each channel chunk
 *   ..  payload: the chunks, back to back
 *
 * Each chunk is range coded and starts with its own range-coded header, so a
 * chunk is not byte aligned and cannot be found by seeking: the length table
 * above is what makes random access possible.  Chunk header, in order:
 *
 *   peak_q  u16   channel peak, x /= peak -> x /= 32768
 *   mean_q  i16   channel mean, restored on decode
 *   a1_q    i16   predictor gain, a1 = a1_q / 16384
 *   step_q  u16   quantiser step in normalised units, step = step_q / 32768
 *   bits    u8    must match the container's bits
 *
 * Every one of those header fields is quantised before it is written, and the
 * encoder then codes the already-quantised value.  Rounding on the way out and
 * again on the way back in desynchronises the two sides by one LSB, and because
 * DPCM is recursive that desync persists to the last sample.
 */

#include <stdint.h>
#include <stddef.h>

#include "nofft.h"

#define ACPCM_MAGIC        "NFA1"
#define ACPCM_FIXED_HEADER 20
#define ACPCM_BITS_MIN     2
#define ACPCM_BITS_MAX     24

typedef struct {
    uint16_t bits;       /* quantiser width actually used */
    uint32_t channels;
    uint32_t sample_rate;
    uint64_t frames;     /* frames per channel */
    uint64_t payload;    /* bytes of range coded payload */
    uint64_t total;      /* whole file size */
} acpcm_info;

/* p->samples is interleaved.  `bits` outside ACPPCM_BITS_MIN..ACPCM_BITS_MAX is
   rejected with ERR_RANGE. */
Err acpcm_encode(const char* path, const pcm_buf* p, uint16_t bits);

Err acpcm_decode(const char* path, pcm_buf* out);

/* Header only, no decoding. */
Err acpcm_file_info(const char* path, acpcm_info* out);

#endif