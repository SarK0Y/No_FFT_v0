# Streaming decode: sample sinks

By default the decoders unpack the whole clip into a `pcm_buf` and only then can
anything look at it (`nofft_decode`, `acpcm_decode`, then `wav_save`). A **sample
sink** flips that around: samples are handed to the caller as they are produced,
so output starts flowing *before* the unpack finishes and the whole clip is never
held in memory.

Two decoders support sinks:

- `nofft_decode_sink` / `nofft_decode_buf_sink` — the poly (`.no_fft`) decoder
- `acpcm_decode_sink` — the acpcm (`.nadc`) decoder

Both deliver exactly the samples the buffered calls return; only *when* you get
them changes. A streaming WAV write is byte-for-byte identical to
`nofft_decode` + `wav_save`.

## The sink

```c
typedef struct nofft_sink {
    void* user;
    Err (*begin)(void* user, uint32_t sample_rate, uint16_t channels,
                 size_t total_samples);
    Err (*write)(void* user, const float* samples, size_t count);
} nofft_sink;
```

- `begin` runs **once**, before any samples. It reports the format and the total
  sample count, both already known from the container header, so a sink can size
  a header or preallocate. It may be `NULL`.
- `write` runs once per decoded frame (poly) or per chunk (acpcm) with
  interleaved `float` samples (`count` is `frames * channels`). It may be `NULL`.
- Either callback returns `ERR_OK` to continue. Any other value aborts the decode
  and is returned to the caller unchanged.
- `user` is passed straight through to both callbacks; put your state there.

## Streaming to a WAV file

`nofft_wav_sink` is a ready-made sink that writes a WAV file as samples arrive,
using `drwav` (already used by `wav_save`). `drwav` patches the RIFF/data sizes
when the writer is released, so the file is valid once `nofft_wav_sink_fini`
runs.

```c
#include "nofft.h"

nofft_wav_sink ws;
nofft_wav_sink_init(&ws, "out.wav");

Err e = nofft_decode_sink("in.no_fft", &ws.sink);
nofft_wav_sink_fini(&ws);          /* always call this: it finalises the file */

if (e != ERR_OK) { /* remove("out.wav"); ... */ }
else printf("%u ch, %u Hz, %zu samples\n",
            ws.channels, ws.sample_rate, ws.total);
```

Fields filled in by `begin` (handy for a summary line after decoding):

| field         | meaning                                  |
|---------------|------------------------------------------|
| `channels`    | channel count                            |
| `sample_rate` | sample rate in Hz                        |
| `total`       | total interleaved samples expected       |

For acpcm, the same sink is used with the other decoder:

```c
nofft_wav_sink ws;
nofft_wav_sink_init(&ws, "out.wav");
Err e = acpcm_decode_sink("in.nadc", &ws.sink);
nofft_wav_sink_fini(&ws);
```

If decoding fails after `begin` has opened the file, a short, still-readable WAV
is left behind. Remove it yourself if you do not want partial output.

## A custom sink

Use your own callbacks to push samples somewhere other than a WAV file — a ring
buffer, a network stream, a resampler, an audio device. Hold the state in a
struct and point `user` at it:

```c
typedef struct {
    FILE* f;
    uint16_t ch;
    size_t   written;
} my_sink;

static Err on_begin(void* user, uint32_t sr, uint16_t ch, size_t total)
{
    my_sink* s = (my_sink*)user;
    s->ch = ch;
    s->written = 0;
    (void)sr; (void)total;
    return ERR_OK;
}

static Err on_write(void* user, const float* samples, size_t count)
{
    my_sink* s = (my_sink*)user;
    if (fwrite(samples, sizeof(float), count, s->f) != count)
        return ERR_IO;
    s->written += count;
    return ERR_OK;
}

/* ... */
my_sink st = { .f = fp };
nofft_sink sink = { .user = &st, .begin = on_begin, .write = on_write };
Err e = nofft_decode_sink("in.no_fft", &sink);
```

Declare the callback types in C-compatible code: `nofft_sink` is a plain C
struct and both decoders have a C ABI.

## Choosing buffered vs. streaming

| want                                   | use                                      |
|----------------------------------------|------------------------------------------|
| all samples at once, random access     | `nofft_decode` / `acpcm_decode`          |
| write output while decoding            | `*_decode_sink` with `nofft_wav_sink`    |
| feed a device / network / own pipeline | `*_decode_sink` with custom callbacks    |
| decode from memory, not a file (poly)  | `nofft_decode_buf_sink`                  |

Streaming holds one frame (poly) or a small fixed chunk (acpcm) at a time, so a
multi-hour file decodes in constant memory instead of `samples * sizeof(float)`.

## Notes

- The total sample count is known up front from the header; `begin` reports it
  before the first `write`. There is no need to buffer in order to size output.
- Sink errors stop the decode immediately; the decoder frees its internal state
  and returns your error code.
- NFM-wrapped inputs are unwrapped by the CLI before decoding; the sink API
  itself takes the raw container the decoder expects (same as the buffered
  calls).
