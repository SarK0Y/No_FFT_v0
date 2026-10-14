# NFM container (No_FFT Media)

The **NFM** format is a simple, little-endian, stream-friendly container that can
bundle one of the No_FFT algorithms together with arbitrary textual metadata and
an optional PNG/JPEG image (a cover). Its purpose is to keep a compressed audio
stream and its provenance in a single, self-describing file.

Files use extension `.nfm` and start with magic `"NFM\0"` (or just `"NFM"` with
a version byte). The layout is designed so a decoder can scan fields until
`END_HEAD`, then read the raw compressed payload produced by the selected codec.

## Magic and header

All integers are little endian.

```text
+---------------------+
| magic[4] = "NFM"   |
| version  (uint8_t) |  (currently 1)
| reserved (uint8_t) |
| flags    (uint16_t)|
+---------------------+
```

`flags` is reserved for future use and must be 0 in v1.

## Text fields

Any number of UTF-8 text fields may follow. Each field is encoded as:

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

## Optional image

An optional cover image may be embedded. If present it appears once before
`END_HEAD`. The image block:

```text
+-------------------+
| img_type (uint8_t)|
| img_len  (uint32_t)|
| img_data [img_len]|
+-------------------+
```

`img_type` values:
- `0` = no image (not used when block is present)
- `1` = PNG
- `2` = JPEG

The data is the raw bytes of the image file (not base64).

## END_HEAD marker

The header ends with a fixed marker:

```text
+------------------------+
| end_head[8] = "END_HEAD"|
+------------------------+
```

Everything after `END_HEAD` is the **inner payload**: the exact byte stream
produced by the chosen codec, written as-is. For `poly` this means the `.no_fft`
container bytes; for `nfa1..nfa6` this means the `.nadc` container bytes (which
start with `NFA1`..`NFA6`). A decoder can detect the inner format by reading the
first 4 bytes of the payload.

## Recommended behavior

- Writing: emit NFM header, write any desired text/image fields, write
  `END_HEAD`, append the codec's encoded bytes.
- Reading: read up to `END_HEAD` (or scan until it appears), extract metadata and
  optional image, then treat the remainder as the inner codec stream and decode
  with the appropriate codec (based on inner magic or on the `codec` text field).
- Preservation: when copying/re-encoding, copy unknown fields and image.
- Streaming: the format is seek-forward-friendly in the header (all lengths are
  explicit) and the payload is a single blob.

## Minimal example (conceptual)

```text
NFM\0  v1  flags=0
name_len=5 "title" value_len=12 "Test Track"
img_type=1 img_len=... <png bytes>
END_HEAD
<NFA3 or NOFF container bytes>
```

## Using with the codec registry

The in-memory APIs in `codec.h` produce the inner payload bytes. A tool can wrap
them in NFM when writing a single `.nfm` file, or unwrap NFM when reading. The
existing `.no_fft` and `.nadc` files remain valid (they have different magic and
no NFM header) so backward compatibility is preserved.

See also: [modules.md](./modules.md), [nofft.md](./nofft.md), and the per-format
docs for `nfa1`..`nfa6`.
