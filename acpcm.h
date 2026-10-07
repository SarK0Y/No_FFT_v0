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
 *
 * NFA2 (magic "NFA2") is the same codec with a forward adaptive predictor.
 * The fixed header gains uint32 block_len at offset 20 (24 bytes total) and
 * the length table starts there instead of at 20.  Inside every chunk, before
 * the sample at i = k * block_len, the encoder sends one adaptive binary flag
 * and, when the flag is set, a new 16 bit a1_q that replaces the predictor
 * gain for the rest of the chunk.  The encoder decides the flags (policy:
 * never / always / drift); the decoder only obeys.  NFA1 files carry
 * block_len = 0 and no flags, so both formats share one decoder.
 */

#include <stdint.h>
#include <stddef.h>

#include "nofft.h"

#define ACPCM_MAGIC         "NFA1"
#define ACPCM_MAGIC2        "NFA2"
#define ACPCM_FIXED_HEADER  20
#define ACPCM_FIXED_HEADER2 24
#define ACPCM_BITS_MIN      2
#define ACPCM_BITS_MAX      24

/* NFA2: when does the encoder refit a1 at a block boundary?  NEVER keeps the
   stream bit identical to NFA1 (flags all zero); the refit itself is decided
   encoder side, the decoder only reads what was sent. */
typedef enum {
    ACP2_NEVER  = 0,
    ACP2_ALWAYS = 1,
    ACP2_DRIFT  = 2
} acpcm_p2_policy;

typedef struct {
    uint16_t bits;       /* quantiser width actually used */
    uint32_t channels;
    uint32_t sample_rate;
    uint32_t block_len;  /* NFA2 block length in frames, 0 for NFA1 */
    uint64_t frames;     /* frames per channel */
    uint64_t payload;    /* bytes of range coded payload */
    uint64_t total;      /* whole file size */
} acpcm_info;

/* p->samples is interleaved.  `bits` outside ACPCM_BITS_MAX..ACPCM_BITS_MAX is
   rejected with ERR_RANGE. */
Err acpcm_encode(const char* path, const pcm_buf* p, uint16_t bits);

/* NFA2 with refits every block_len frames; block_len 0 and a policy outside
   NEVER..DRIFT are rejected with ERR_RANGE. */
Err acpcm_encode2(const char* path, const pcm_buf* p, uint16_t bits,
                  uint32_t block_len, acpcm_p2_policy policy);

Err acpcm_decode(const char* path, pcm_buf* out);

/* Header only, no decoding. */
Err acpcm_file_info(const char* path, acpcm_info* out);

/* ---- streaming decode ----
 *
 * Every channel is its own range-coded stream, so a stereo file cannot be
 * decoded one channel at a time and stitched together afterwards without
 * buffering the whole thing.  This advances all channels in lockstep and
 * interleaves them as it goes, which is what makes bounded-memory playback
 * possible.
 */
struct AcpcmDecoder;

/* Reads the container and both channels' headers; no audio is decoded yet. */
Err acpcm_dec_open(const char* path, AcpcmDecoder** out);
void acpcm_dec_close(AcpcmDecoder* d);
const acpcm_info* acpcm_dec_info(const AcpcmDecoder* d);

/* Decodes up to max_frames interleaved frames into `out`, which must have room
   for max_frames * channels floats.  *got is the number of frames produced; a
   short count means end of stream.  Memory use does not depend on file length. */
Err acpcm_dec_read(AcpcmDecoder* d, float* out, size_t max_frames, size_t* got);

/* ---- playback ----
 *
 * Decodes straight to ALSA, so a long file does not have to be held in memory.
 * `device` may be NULL for the default.  Built without ALSA the call reports
 * ERR_UNSUPPORTED_FMT and the codec still encodes and decodes.
 */
Err acpcm_play(const char* path, const char* device);

/* ---- test signal ----
 *
 * A deterministic stereo/mono tone-and-chirp WAV, so the codec can be checked
 * without shipping an input file.
 */
Err acpcm_gen(const char* path, double seconds, uint32_t channels, uint32_t sample_rate);

#endif