# No_FFT

A lossy audio codec for `.no_fft` files that fits a **polynomial** to short blocks of
a waveform. No FFT is involved, at any stage.

Each block of `frame_len` frames is summarised by the coefficients of a polynomial
that passes through a small number of chosen samples. The decoder evaluates that
polynomial to rebuild the block. Because a block is described by `degree + 1`
numbers instead of `frame_len` samples, the file is smaller than the PCM it came
from, and the size/fidelity trade is yours to tune with `frame_len` and `degree`.

This is an experiment in time-domain polynomial fitting, not a competitor to
lossless codecs. It is lossy by construction.

## Build

```sh
make            # host build -> codecs/linux/ubuntu/nofft
make deb        # Debian package -> build/nofft_<version>_amd64.deb
make windows    # cross-build win64 (needs mingw-w64)
make checkver   # assert control/changelog/man agree on the version
make clean
```

Only `main.cpp`, `nofft.cpp`, `nofft.h` and the vendored `third_party/dr_wav.h`
are compiled. There are no other dependencies.

## Install

```sh
sudo dpkg -i build/nofft_0.2.0_amd64.deb
```

Or skip the package and just run the binary from `codecs/linux/ubuntu/`.

## Use

```
nofft encode   <in.wav>    <out.no.fft> [frame_len] [degree] [f32|f16]
nofft decode   <in.no.fft> <out.wav>
nofft roundtrip <in.wav>              [frame_len] [degree] [f32|f16]
nofft play     <in.no.fft>
```

`roundtrip` is the thing to reach for while tuning: it encodes, decodes, and
prints the resulting SNR without touching the disk.

```sh
nofft roundtrip song.wav 20 4        # defaults: frame_len 20, degree 3, f32
nofft encode song.wav song.no_fft 20 4 f16
nofft play song.no_fft | mpv -
```

### Playing it

`nofft play` writes a canonical 44-byte-header RIFF/WAVE stream of IEEE float32
to **stdout**, with every diagnostic on **stderr**, so stdout stays pure audio:

```sh
nofft play song.no_fft | mpv -
nofft play song.no_fft | vlc -
nofft play song.no_fft | ffplay -
```

There is deliberately no plugin. Neither mpv (which backs SMPlayer) nor VLC
exposes an audio-decoder plugin API that a distributable package can register, so
a pipe is the supported route rather than a workaround. Getting real players to
open `.no_fft` natively would mean shipping an FFmpeg/libavcodec decoder.

### Tuning

| Knob | Effect |
| --- | --- |
| `frame_len` | Frames per block. The dominant size control: bigger blocks mean fewer polynomials. |
| `degree` | Coefficients per block. More coefficients means a better fit at a larger cost. |
| `f32` / `f16` | 4 or 2 bytes per coefficient. `f16` halves the file. |

Effective degree is clamped to `frame_len - 1`, and refused above 63 (the solver
holds at most 64 coefficients). Blocks are all `frame_len` except the last, which
may be shorter.

Measured on a 44100-frame stereo file, `degree=6`:

| `frame_len` | ratio (f16) | SNR |
| --- | --- | --- |
| 10 | 1.43:1 | 62.08 dB |
| 20 | 2.86:1 | 69.71 dB |
| 40 | 5.70:1 | 45.36 dB |
| 80 | 11.38:1 | 6.65 dB |

The cliff is steep, and it arrives earlier than intuition suggests. Larger
`frame_len` buys ratio and loses the waveform.

### f16 and degree

`f16` is IEEE binary16, and it keeps only an 11-bit significand. At low and
moderate degrees that is invisible. At high degrees the high-order coefficients
get large, quantise badly, and reconstruction collapses — at `degree=19`, f32
reaches 88.10 dB while f16 reaches 9.27 dB.

So the encoder measures the reconstruction f16 would actually produce and warns on
stderr when f32 would have been at least 6 dB better:

```
nofft: warning: f16 coefficients reconstruct at 9.3 dB SNR here
(f32 would give 249.0 dB); prefer f32 for this degree
```

The encode still succeeds; the warning is there because this failure is otherwise
silent. Coefficients outside the binary16 range of ±65504 are refused outright
rather than saturated.

## How it works

For each block of `frame_len` frames, independently per channel:

1. Split the block into `degree + 1` sub-ranges.
2. For each sub-range, take its mean and keep the single sample nearest to it.
   That sample's position and value become one constraint point.
3. Map positions to `x` in `[-1, 1]`, where `x = 2*pos/(len-1) - 1`.
4. Solve the square Vandermonde system for the coefficients by Gaussian
   elimination with partial pivoting.
5. Store the coefficients.

Decoding is a Horner evaluation of that polynomial at each frame position.

Two consequences worth knowing:

- **The fit is not a least-squares fit.** It interpolates chosen samples
  exactly and ignores the rest, which is why it stays square and cheap but also
  why high degrees can behave badly.
- **The sub-range count is `degree + 1`, not `degree`.** This differs from the
  original specification for the project, which called for `degree` sub-ranges
  plus frame-start and fixed-median positions. This is a known, unresolved
  discrepancy.

## Format

32-byte header, all little-endian, then bare coefficients with no per-block
length prefix. Only the final block may be short, and the header records its
length.

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | magic `NOFF` |
| 4 | 4 | version (3) |
| 8 | 4 | sample rate |
| 12 | 2 | channels |
| 14 | 2 | degree |
| 16 | 4 | frame_len |
| 20 | 4 | block count |
| 24 | 4 | length of the final block |
| 28 | 1 | coefficient format: 0 = f32, 1 = f16 |
| 29 | 3 | padding |

Version 2 files are still read, as f32. Version 1 (which carried a length per
block) is rejected, as is any unrecognised coefficient format.

**Known portability defect:** the header and f32 coefficients are written with
host byte order, so files produced on a big-endian machine will not read on a
little-endian one and vice versa. f16 coefficients are little-endian. Little-
endian only, in practice.

## Library API

`nofft.h` is usable directly. Errors are returned as an `enum Err` and rendered
through the `g_err_str` function pointer, so you can localise or reformat them.

```c
pcm_buf p;
if (wav_load("song.wav", &p) != ERR_OK)
    return 1;

if (nofft_encode("song.no_fft", &p, 20, 4, NOFFT_COEF_F16) != ERR_OK)
    return 1;

pcm_free(&p);
```

Also exported: `wav_save`, `wav_write_stream`, `nofft_decode`,
`nofft_decode_snr_db`, `nofft_coeff_format_str` / `_parse`, and the solver
(`nofft_solve_vandermonde`, `nofft_eval_poly`) with its helpers.

The code is C++ written in a C style: no classes, no STL containers, no
exceptions, manual allocation, `enum Err` returns. Buffers are freed with
`pcm_free`.

## Supported input

WAV only, read through dr_wav: PCM, IEEE float, A-law and mu-law. Output is
always 32-bit float WAV.

## Verification

Built with `-Wall -Wextra -Werror -pedantic`. The f16 conversion is checked
exhaustively: all 65536 binary16 values round-trip, adjacent float values land
within 1 ulp, and exact ties round to even. ASan, UBSan and leak detection are
clean across degrees 0–63, frame lengths 1–200, both coefficient formats, mono /
stereo / float input, and the corrupt-file and error paths.

## Layout

```
main.cpp        CLI
nofft.cpp       WAV I/O, interpolation, solver, f16 conversion, container
nofft.h         public API and format constants
Makefile        host / windows / deb targets
debian/         packaging: control, copyright, changelog, nofft.1
third_party/    dr_wav.h (vendored)
codecs/         build output per platform
build/          packages and staging
```

`github/` is a separate nested checkout and is not kept in sync with the root.

## Licence

See `LICENSE`.

## Status

Working, with two known issues: the host-endian serialisation defect above, and
the sub-range discrepancy with the original specification. Neither blocks normal
use on little-endian machines.