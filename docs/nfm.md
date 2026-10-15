# NFM container (No_FFT Media)

The **NFM** format is a simple, little-endian, length-prefixed container that can
bundle any No_FFT codec payload with optional text metadata and an optional
PNG/JPEG image (cover). Files use extension `.nfm` and start with magic `nfm`
(3 bytes).

## Header structure

```text
+---------------------+
| magic[3] = "nfm"   |
| header_len (uint64_t)|
+---------------------+
```

`header_len` is the length in bytes of the header data that follows. The header
data always begins with an image block (possibly empty), followed by zero or
more text fields:

```text
+-------------------+
| img_type (uint8_t)|
| img_len  (uint32_t)|
| img_data [img_len]|   (only when img_len > 0)
+-------------------+
| text field 0      |
| text field 1      |
| ...               |
+-------------------+
```

Because the image block is always present, a reader can unambiguously tell where
it ends and the text fields begin. `img_len == 0` means "no image".

## Text fields

Text fields are encoded inside the header data as:

```text
+--------------------+
| name_len (uint16_t)|
| name     [name_len]|
| value_len(uint32_t)|
| value    [value_len]|
+--------------------+
```

Names are short ASCII/UTF-8 keys (e.g. `title`, `artist`, `album`, `comment`,
`encoder`, `date`). Values may be arbitrary UTF-8 text. There is no fixed
dictionary; unknown fields must be preserved if rewriting.

Common keys suggested (not enforced):
- `title`, `artist`, `album`, `track`, `date`, `genre`
- `codec` — name of the inner codec (e.g. `poly`, `nfa1`, `nfa2`, `nfa3`,
  `nfa4`, `nfa5`, `nfa6`)
- `comment`, `encoder`, `software`

## Image block

The image block is always the first part of the header data. When an image is
present (`img_len > 0`) it is encoded as:

```text
+-------------------+
| img_type (uint8_t)|
| img_len  (uint32_t)|
| img_data [img_len]|
+-------------------+
```

`img_type` values:
- `0` = none
- `1` = PNG
- `2` = JPEG

The data is the raw bytes of the image file (not base64). When there is no
image, `img_type = 0` and `img_len = 0`.

## Payload

Everything after the header (header data as specified by `header_len`) is the
**inner payload**: the exact byte stream produced by the chosen codec, written
as-is. For `poly` this means the `.no_fft` container bytes; for `nfa1..nfa6`
this means the `.nadc` container bytes (which start with `NFA1`..`NFA6`). A
decoder can detect the inner format by reading the first 4 bytes of the payload.

## Recommended behavior

- Writing: emit magic `nfm`, write `header_len` (u64), write the image block
  (always present; `img_len = 0` when none), write the text fields, then append
  the codec's encoded bytes.
- Reading: read magic + header_len, read the image block, then parse the text
  fields to extract metadata, treat the remainder as the inner codec stream and
  decode with the appropriate codec.
- Preservation: when copying/re-encoding, copy unknown fields and image.
- Streaming: the format is seek-forward-friendly via length prefixes.

## Creating NFM files

The CLI can create NFM files during conversion. When `-nfm` is specified,
`convert`/`convert-all` wrap the encoded output in an NFM container and set the
output extension to `.nfm`.

### Example (convert an MP3/WAV to NFM)

```bash
./nofft convert input.mp3 output.nfm -algo nfa3 -bits 6 -nfm
```

Metadata (title, artist, album) and the cover image are automatically extracted
from the source file when available. Override or add text fields explicitly:

```bash
./nofft convert input.mp3 output.nfm -algo nfa3 -bits 6 -nfm \
  -nfm-title "Song Title" \
  -nfm-artist "Artist Name" \
  -nfm-album "Album Name"
```

Flags:
- `-nfm` — enable NFM output (forces `.nfm` extension)
- `-nfm-title <text>` — set title (overrides extracted value)
- `-nfm-artist <text>` — set artist (overrides extracted value)
- `-nfm-album <text>` — set album (overrides extracted value)

### Automatic cover-art copying

When `-nfm` is set, the source file's attached picture (ID3 `APIC` cover in an
MP3, or an embedded image in a WAV) is extracted with `ffmpeg` and stored in the
NFM header as a PNG. Any image format ffmpeg can decode (e.g. JPEG/MJPEG cover
art) is converted to PNG before embedding. Sources without cover art simply get
an empty image block (`img_type = 0`, `img_len = 0`).

Requires `ffmpeg` on `PATH`; if it is missing or the file has no cover, the NFM
is still written without an image.

With `convert-all`, each codec produces `basename.<codec>.nfm` when `-nfm` is
set.

### Playing and decoding NFM

NFM files are auto-detected. The CLI unwraps them transparently for playback and
decoding:

```bash
./nofft play song.nfm           # play directly (nofft or ACPCM inside)
./nofft decode song.nfm out.wav # decode poly/NOFF payload to WAV
./nofft ac-play song.nfm        # play ACPCM (.nadc) payload to stdout/WAV stream
./nofft ac-decode song.nfm out.wav # decode ACPCM payload to WAV
```

### Library API

The C API can also write NFM containers directly:

```c
#include "nfm.h"

nfm_meta meta = { .title = "Title", .artist = "Artist", .album = "Album" };
Err e = nfm_write("out.nfm", payload, payload_len, &meta, img, img_len, NFM_IMG_PNG);
```

## Minimal example (conceptual)

```text
nfm  header_len=...
img_type=1 img_len=... <png bytes>
name_len=5 "title" value_len=12 "Test Track"
<NFA3 or NOFF container bytes>
```

## Using with the codec registry

The in-memory APIs in `codec.h` produce the inner payload bytes. A tool can wrap
them in NFM when writing a single `.nfm` file, or unwrap NFM when reading. The
existing `.no_fft` and `.nadc` files remain valid (they have different magic and
no NFM header) so backward compatibility is preserved.

See also: [modules.md](./modules.md), [nofft.md](./nofft.md), and the per-format
docs for `nfa1`..`nfa6`.
