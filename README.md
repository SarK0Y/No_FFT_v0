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
make            # host build -> codecs/linux/ubuntu/{nofft,acc}
make deb        # Debian package -> build/nofft_<version>_amd64.deb
make windows    # cross-build win64 (needs mingw-w64)
make checkver   # assert control/changelog/man agree on the version
make test       # both self tests, no input files needed
make check-ac   # prove the C and C++ codecs agree byte for byte
make clean
```

Two binaries are built. `nofft` is C++ and is the format reference. `acc` is the
same acpcm codec in portable C11, with no dependencies beyond libc (and ALSA for
playback, detected by `pkg-config`; without it `acc` still encodes and decodes).
Only `main.cpp`, `nofft.cpp`, `acpcm.cpp`, `selftest.cpp`, the two headers and
the vendored `third_party/dr_wav.h` are compiled for `nofft`; there are no other
dependencies.

## Install

```sh
sudo dpkg -i build/nofft_0.2.0_amd64.deb
```

Or skip the package and just run the binary from `codecs/linux/ubuntu/`.

## Use

```
nofft encode   <in.wav>    <out.no.fft> [frame_len] [degree] [f32|f16] [interp|least-sq]
nofft decode   <in.no.fft> <out.wav>
nofft roundtrip <in.wav>              [frame_len] [degree] [f32|f16] [interp|least-sq]
nofft convert  <in.mp3> <out.no.fft>  [frame_len] [degree] [f32|f16] [interp|least-sq]
nofft play     <in.no.fft>

nofft ac-encode   <in.wav> <out.nadc> [bits] [-snr] [-block N] [-policy never|always|drift] [-v3] [-v4 -s N]
nofft ac-decode   <in.nadc> <out.wav>
nofft ac-roundtrip <in.wav>            [bits] [-block N] [-policy never|always|drift] [-v3] [-v4 -s N]
nofft ac-info     <in.nadc>
nofft ac-play     <in.nadc>
nofft selftest
```

`roundtrip` is the thing to reach for while tuning: it encodes, decodes, and
prints the resulting SNR without touching the disk.

```sh
nofft roundtrip song.wav 20 4        # defaults: frame_len 20, degree 3, f32
nofft encode song.wav song.no_fft 20 4 f16
nofft convert song.mp3 song.no_fft 20 4 f16
nofft play song.no_fft | mpv -
```

### From MP3

`convert` takes anything `ffmpeg` can read, MP3 included, decodes it to float32
WAV in a temporary file, and encodes that:

```sh
nofft convert song.mp3 song.no_fft 20 4 f16
```

The intermediate WAV is removed on every path, including failures. Filenames go
straight to `ffmpeg` via `execvp`, never through a shell, so spaces and shell
metacharacters in names are safe.

### What MP3 cannot be represented by

Read this before trusting converted output. The codec suits smooth, tonal
material, and MP3 is not that. Measured SNR through a full decode at
`frame_len=20 degree=6 f32`:

| Input | SNR |
| --- | --- |
| 440 Hz sine | 72.83 dB |
| four-tone mix (440/1319/3111/7000 Hz) | -2.66 dB |
| pink noise | -4.38 dB |

Negative SNR means the reconstruction is noisier than the original, which is
what Runge divergence looks like when a polynomial is asked to follow content it
cannot. Sweeping pink noise across settings does not rescue it: wherever real
compression happens, at ratios above roughly 1.2:1, the SNR is negative.
Fidelity and compression only coexist once the degree is high enough that the
file is no smaller than the PCM, at which point there is no reason to use this
codec.

So `convert` handles MP3 mechanically and correctly, but this is not a viable
MP3 transcoder for real music. It suits synthetic or strongly tonal material.
Check `nofft roundtrip` on your material before converting a whole library.
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
| `interp` / `least-sq` | How the polynomial is fitted. `least-sq` fits every sample in the block; `interp` interpolates picked samples. Default `interp`. |

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

For each block of `frame_len` frames, independently per channel, with `interp`:

1. Split the block into `degree + 1` sub-ranges.
2. For each sub-range, take its mean and keep the single sample nearest to it.
   That sample's position and value become one constraint point.
3. Map positions to `x` in `[-1, 1]`, where `x = 2*pos/(len-1) - 1`.
4. Solve the square Vandermonde system for the coefficients by Gaussian
   elimination with partial pivoting.
5. Store the coefficients.

Decoding is a Horner evaluation of that polynomial at each frame position.

### `least-sq`

`interp` interpolates a handful of picked samples exactly and ignores the rest.
That keeps the system square and cheap, but it also means a block whose unpicked
samples are wild gets fitted through a couple of outliers and ignores everything
else. `least-sq` uses every sample in the block as one equation and minimises the
sum of squared error instead:

1. Map each frame position to `x` in `[-1, 1]`, the same grid the decoder uses.
2. Build one row of `A` per sample, with `A[k][j] = x_k^j`.
3. Solve the over-determined system `min ||Ax - y||` by Householder QR.
4. Store the coefficients.

The solver is QR, not the normal equations. `A'A` squares the condition number of
this Vandermonde, which is already large: by around degree 20 the normal equations
return visibly wrong answers while QR stays accurate to about `1e-14` relative
even at degree 63. There is no basis change, so the stored coefficients remain
plain monomials and the decoder is unchanged.

Cost is one QR per block instead of one `O(degree^3)` solve, and the encoder
allocates two `frame_len`-sized scratch buffers, so `least-sq` is the slower path.
That is a CPU/quality trade, not a size trade: for the same `frame_len`, `degree`
and coefficient format, both fits produce byte-identical file sizes.

Because a least-squares fit has enough equations, it does not overshoot the way
interpolation does. Reconstruction error drops at every setting measured:

| Source (44.1 kHz, from MP3) | `frame_len` | `degree` | `interp` | `least-sq` |
| --- | --- | --- | --- | --- |
| sine | 20 | 6 | 72.84 dB | 81.09 dB |
| harmonic stack | 20 | 6 | 82.29 dB | 89.34 dB |
| vibrato | 20 | 6 | 5.01 dB | 17.68 dB |
| pink noise | 20 | 6 | -4.66 dB | 9.07 dB |
| white noise | 20 | 6 | -11.33 dB | 2.08 dB |

The gain is small on clean periodic material and large on anything noisy or
amplitude-modulated, which is where `interp` was weakest. `least-sq` does not fix
the underlying problem with this codec — it is still a per-block polynomial fit,
and broadband material remains poorly served. It removes a failure mode rather
than the limitation.

One caveat: when `degree + 1 >= frame_len` the system is square, so `least-sq`
and `interp` solve the same interpolation problem and agree in exact arithmetic.
Observed differences there are f32 coefficient rounding of an ill-conditioned
high-degree fit, not a difference in method.

Two further notes on `interp`:

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
| 4 | 4 | version (6) |
| 8 | 4 | sample rate |
| 12 | 2 | channels |
| 14 | 2 | degree |
| 16 | 4 | frame_len |
| 20 | 4 | block count |
| 24 | 4 | length of the final block |
| 28 | 1 | coefficient format: 0 = f32, 1 = f16 |
| 29 | 1 | fit: 0 = interp, 1 = least-sq |
| 30 | 2 | padding |

### Per-block statistical fill (versions 5 and 6)

Each block begins with one extra byte, then the coefficients:

    [fill byte][channel 0 coefficients][channel 1 coefficients]...

The fill byte is the block's residual RMS on a log scale over `[-140, 0] dBFS`
(255 steps). Byte `0` means no fill and is emitted for exact fits and for digital
silence. The decoder regenerates deterministic pseudo-random noise from the block
index, scales it to the decoded RMS, and adds it to the polynomial, so the
reconstruction carries the same noise character as the original block. The noise
is derived purely from the block index, so no seed is stored and encoding is
reproducible.

This costs one byte per block: at `L=32, D=6, f16` stereo a block grows from 28 to
29 bytes, i.e. 3.5 to 3.625 bits per sample (+3.6%).

### Per-block bounds (version 6, current)

Version 6 replaces the RMS byte with **two** bound bytes, so its block is
`[min][max][coefficients]`:

    [min][max][channel 0 coefficients][channel 1 coefficients]...

Each bound is a signed log-magnitude byte over `[-60, 0] dBFS` with the sign in
bit 7; `0` means exact zero. The decoder pulls any reconstructed sample that
falls outside `[min, max]` back by a random fraction (50-100%) of its overshoot,
repeating up to four times. This converges very close to a hard clamp while
leaving a little random residue.

At `L=32, D=6, f16` stereo a block is 30 bytes, i.e. 3.75 bits per sample
(+7.1% over version 4). The bounds correct 1.9% of samples, and about 55% of
blocks contain at least one such sample.

**Version 6 replaced version 5 because the two are not combinable.** Bounds pull
inward on the samples the polynomial already overshoots; an RMS fill adds energy
everywhere. Bounds make the error slightly more tonal, the fill makes it slightly
less tonal, and only one of the two is worth its bytes.

Versions 2, 3, 4 and 5 are still read. Version 5 keeps its one RMS byte, version
4 is the first fit-aware version and has no per-block side info; both are read
identically apart from their leading byte.
Version 2 predates the coefficient format byte and is read as f32; version 3
predates the fit byte and is read as `interp`. Version 1 (which carried a length
per block) is rejected, as is any unrecognised coefficient format or fit.

**Known portability defect:** the header and f32 coefficients are written with
host byte order, so files produced on a big-endian machine will not read on a
little-endian one and vice versa. f16 coefficients are little-endian. Little-
endian only, in practice.

## acpcm: sample-domain DPCM with a range coder

The commands above and the rest of this file describe the polynomial codec. The
`ac-*` commands are a second, independent codec: it keeps **no polynomial at
all**, and is usually the better choice. Files use the `.nadc` extension —
**N**o-**F**FT **A**udio **D**PCM **C**odec — with the four byte magic `NFA1`
(**N**o **F**FT **A**udio codec, format revision 1), so the two formats never
collide and both readers stay simple. There is also an experimental `NFA2`
(format revision 2, predictor refitted mid-file), `NFA3` (residual baskets),
and `NFA4` (basket-only, no predictor), described under the headings
below; all four magics are read by the same decoder.

```sh
nofft ac-roundtrip song.wav 6     # 30.7 dB at 3.03 bits per sample
nofft ac-encode song.wav song.nadc 6
nofft ac-roundtrip song.wav 6 -block 1024 -policy drift   # NFA2 refits
nofft ac-roundtrip song.wav 6 -v3                        # NFA3 baskets
nofft ac-roundtrip song.wav 8 -v4 -s 5                   # NFA4 baskets (no predictor)
nofft ac-decode song.nadc song.out.wav
nofft ac-play song.nadc | mpv -
```

The same verbs exist in the C build as `acc encode`, `acc decode`, `acc play`,
`acc info`, `acc roundtrip` and `acc selftest`, and they read and write the same
files byte for byte:

```sh
acc encode song.wav song.nadc 6
acc play song.nadc -d null        # render without hardware
acc info song.nadc
acc gen test.wav 0.5 2            # deterministic signal, for testing
```

Two things are done better in C. Channels are separate range-coded streams, so
`acc play` advances them in lockstep and interleaves them as it goes, which
keeps memory at one block regardless of file length; the C++ `ac-decode`
materialises a whole channel at a time. And the C build has its own WAV reader,
so it needs no `dr_wav.h` and no `ffmpeg`.

### How it works

Each channel is coded as a first difference against the *previous reconstructed*
sample:

    xhat[n] = a1 * xhat[n-1] + q[n] * step

so `q` is a small integer and the decoder only needs `a1`, `step` and the
sequence of `q`. Three passes per channel: measure the peak and mean, fit `a1`
by least squares and measure the largest residual, then encode.

The step is derived from that residual peak, `step = 2 * residual_peak / (2^bits
- 1)`, which puts the largest residual just inside the quantiser range. The
symbols go through an LZMA-style carryless range coder: a zero flag, a unary
magnitude class, the mantissa bits, then the sign, each with its own adaptive
probability.

Measured on one bass-heavy 44.1 kHz stereo track, the 47.55 s window available
for testing:

| bits | bits/sample | SNR |
| ---- | ----------- | ------ |
| 4    | 1.21        | 16.8 dB |
| 5    | 2.07        | 24.1 dB |
| 6    | 3.03        | 30.7 dB |
| 7    | 4.03        | 37.0 dB |
| 8    | 5.02        | 43.1 dB |
| 12   | 8.95        | 67.0 dB |
| 16   | 12.27       | 87.0 dB |

Roughly 6 dB per bit, about 20 dB better than the polynomial codec at a
comparable rate.

### Format

All integers little endian, independent of the host.

    0   char     magic[4]   "NFA1"
    4   uint32   frames     frames per channel
    8   uint32   bits       quantiser width, 2..24
    12  uint32   channels
    16  uint32   sample_rate
    20  uint32   len[c]     byte length of each channel chunk
    ..  payload: the chunks, back to back

Every channel is its own range-coded chunk starting with its own range-coded
header: peak, mean, `a1`, step, and `bits`, all 16-bit fixed point except the
width byte. A chunk is therefore not byte aligned and cannot be found by seeking;
the length table is what makes random access possible. Range-coded streams also
cannot be concatenated, which is why each channel is separate rather than one
stream with interleaved channels.

Two details are load-bearing. Every header field is quantised **before** it is
written, and the encoder then codes the already-quantised value: rounding once
more on the way out shifts the gain by one LSB, and because the filter is
recursive the two sides never resynchronise, so the damage runs to the last
sample. And `a1` is held at or below 0.999, since the pole of `1/(1 - a1 z^-1)`
is `a1` and anything above 1 diverges in closed loop.

### NFA2: forward adaptive predictor (experimental)

NFA1 fits `a1` once per channel and holds it for the whole file. When the
material changes character — a quiet passage into percussion, a tone into noise
— that one compromise gain costs bits for the rest of the file. NFA2 lets the
encoder replace `a1` at fixed block boundaries and tell the decoder when it
did. It is enabled per encode with `-block N` (the refit window in frames) and
`-policy never|always|drift` (default `drift`; `-policy` needs `-block`):

```sh
nofft ac-encode song.wav song.nadc 6 -block 1024 -policy drift
nofft ac-roundtrip song.wav 6 -block 1024        # policy defaults to drift
nofft ac-info song.nadc                          # ... block=1024
```

The library API is `acpcm_encode2` with `acpcm_p2_policy`; the decoder needs
nothing new and reads both magics. `ac-info` and the encoder/roundtrip summary
echo `block=` and `policy=` so a stream's shape is visible without a hex
editor.

Container: magic `NFA2`, and the fixed header gains one field, so the length
table starts at 24 instead of 20:

    0   char     magic[4]   "NFA2"
    4   uint32   frames     frames per channel
    8   uint32   bits       quantiser width, 2..24
    12  uint32   channels
    16  uint32   sample_rate
    20  uint32   block_len  frames per refit window, >= 1
    24  uint32   len[c]     byte length of each channel chunk
    ..  payload: the chunks, back to back

Inside a chunk, before the sample at `i = k * block_len`, the encoder sends one
adaptive binary flag; when the flag is 1 it is followed by a new 16-bit `a1_q`
(the same `/16384` fixed point as the chunk header) that replaces the predictor
gain for the rest of the chunk. The flag has its own adaptive probability, so a
run of "no refit" costs close to zero bits. Everything else — chunk header
fields, seed sample, symbols — is byte-for-byte the NFA1 layout, and a
`block_len` of 0 in an NFA2 file is rejected rather than misparsed.

The refit value obeys the same quantise-once rule as every other header field:
the encoder takes the least-squares `a1` over the just-finished block's source
pairs, clamps it to +/-0.999, rounds it into the 16-bit field, and then codes
that rounded integer, so both sides continue with exactly the same gain. A flat
block has nothing to learn and re-emits the current value.

When a refit happens is decided entirely by the encoder; the decoder only reads
what was sent:

| Policy | Refit |
| --- | --- |
| `ACP2_NEVER` | never; the stream decodes bit-identical to NFA1 |
| `ACP2_ALWAYS` | at every block boundary |
| `ACP2_DRIFT` | only when the basket meter reports expensive residuals |

`ACP2_DRIFT` counts the just-finished block's residuals whose magnitude reaches
`lim / 8` of the symbol range; when at least 1% of the block looks that
expensive, the next boundary refits. That is a magnitude meter, so on stable
material it senses busy blocks rather than a wrong predictor and fires more
often than it needs to — the cost is still small.

Measured at `bits=6`, `block_len=1024` (`./measure_acp2`, see Layout):

| Material | NFA1 bits/sample | NFA2 drift bits/sample | change | NFA1 SNR | NFA2 drift SNR |
| --- | --- | --- | --- | --- | --- |
| crafted tone->noise, 10 s (predictor must adapt) | 4.39 | 3.24 | **-26%** | 36.44 dB | 36.52 dB |
| stationary noise, 10 s | 6.06 | 6.08 | +0.3% | 35.54 dB | 35.49 dB |
| real stereo track, 10.4 M frames | 3.207 | 3.220 | +0.4% | 31.22 dB | 31.22 dB |

The win is exactly where `a1` drifts, and it grows with the quantiser width
(-35% at `bits=3`, -17% at `bits=12`); on material whose predictor is already
right the refits cost a few tenths of a percent and nothing measurable in SNR.
`ACP2_NEVER` costs at most +0.0015 bits/sample for the flags themselves. The one
exception is `bits=2`, where the quantiser is too coarse to refit safely and
quality drops; skip refits below `bits=3`. Speed: decoding stays the same or
gets slightly faster (the shorter stream offsets the flag reads), encoding
costs up to ~9% when refits fire on real material and gets *faster* on
drifting material because the stream shrinks.

Reproduce the tables with:

```sh
g++ -std=c++17 -O2 -Wall -Wextra -Werror -pedantic \
    -o measure_acp2 measure_acp2.cpp acpcm_inst.cpp nofft.cpp -lm
./measure_acp2
```

### NFA3: residual baskets (experimental)

NFA1 codes each residual as a unary run (exponent) plus the fixed `bits`
mantissa. When `bits` is large the mantissa is most of the transfer, but the
unary run still spends 2-3 bits on the exponent's tail. NFA3 keeps the NFA1
predictor, quantiser, and chunk header exactly as they are and replaces the
unary run with a **basket index** coded from an adaptive histogram; the
mantissa is unchanged. It is enabled with `-v3` (or `acpcm_encode3`) and is the
simpler of the extra formats: no blocks, no refits, one predictor fit for the
whole file.

The histogram is a first-order context machine: `classes = bits + 1` bins per
row and the same number of rows, keyed by the *previous* basket. A row is
updated after every sample and its counts are halved in scale once the row
total passes 2^20, so the statistics stay agile without a tuning constant.
The bins cover the residual's entire symbol range, but a residual that leaves
the finely measured region can saturate the last bin; when that happens the
encoder emits the last index as an escape followed by the raw 32-bit sample.

Container magic is `NFA3`; the fixed header and each chunk header are
bit-identical to NFA1. Only the per-sample symbol changes, which makes it an
easy thing to measure in isolation:

```sh
nofft ac-encode song.wav song.nadc 6 -v3
nofft ac-roundtrip song.wav 6 -v3
nofft ac-info song.nadc                          # ... format=nfa3 baskets
```

Measured at `bits=6`, one predictor, no refits (`./measure_acp2`):

| Material | NFA1 b/s | NFA3 b/s | NFA3-NFA1 | NFA1 SNR | NFA3 SNR |
| --- | --- | --- | --- | --- | --- |
| crafted tone->noise, 10 s | 4.3855 | 4.4692 | +0.084 | 36.44 dB | 36.44 dB |
| stationary noise, 10 s | 6.0631 | 5.9873 | -0.076 | 35.54 dB | 35.54 dB |
| stationary tone 440, 10 s | 5.8982 | 4.8365 | **-1.062** | 61.39 dB | 61.39 dB |
| tones + noise, 10 s | 5.5816 | 5.5117 | -0.070 | 40.23 dB | 40.23 dB |
| real stereo track (orig.wav) | 3.2070 | 3.2807 | +0.074 | 31.22 dB | 31.22 dB |
| real stereo track (orig1.wav) | 2.3570 | 2.5430 | +0.186 | 25.46 dB | 25.46 dB |
| listen track, 10.4 M frames | 2.0601 | 1.9921 | -0.068 | 27.91 dB | 27.91 dB |

The width ladder (same crafted signal) shows the effect growing with `bits`:
NFA3 beats NFA1 by -0.083 b/s at `bits=7`, -0.223 at `bits=8`, ... -0.413 at
`bits=12`, while SNR is identical at every width (the residual mantissa is
unchanged). Below `bits=6` the table costs a few tenths of a bit instead:
at `bits=4` +0.242, `bits=3` +0.079, and at `bits=2` the escape fires often
enough to cost +1.078. So NFA3 is a pure refinement: same predictor, same
quality, and on narrow (tone-like) residuals it buys back the unary run.

### NFA4: basket-only, no predictor (experimental)

NFA3 asks "is the mantissa-shaped unary run worth replacing?" NFA4 goes the
other way and asks how far one transmitted histogram can get *without* any
predictor or residual at all. Every sample carries exactly one symbol: its
basket index. There is no `a1` filter, no seed sample, no residual.

A basket index alone is a magnitude class, so to avoid a sign symbol the channel
grid is shifted first. The header carries the channel minimum `base_q`; every
sample maps to `u = xq - base_q >= 0`, and the decoder adds `base_q` back. On
that shifted grid `u` is floored onto a `step`-percent grid of the channel range
`peak_q`:

    b = floor(u * 100 / (peak_q * step))

The transmitted codebook is one `[count: 32-bit | deviation: 16-bit float]` row
per basket (the `float16` column is an upper edge, carried and validated but
never used in the arithmetic). The decoder rebuilds the bucket midpoint

    mid_b = (ceil(b*peak_q*step/100) + ceil((b+1)*peak_q*step/100)) / 2

and returns `(base_q + mid_b) / 32768`. A flat or silent channel has `peak_q
== 0`, so every sample lands in bucket 0 whose midpoint is 0 and the output is
`base_q` exactly: silence and DC round-trip bit for bit.

Although the header keeps the `bits` byte for compatibility, the basket grid is
set entirely by `step`; the residual width does not enter (T8 shows the NFA4
columns flat down the `bits` ladder).

Enabled with `-v4 -s step`, where `step` is the bucket width in percent of the
channel range (`-s` needs `-v4`; defaults to `-s 5`):

```sh
nofft ac-encode song.wav song.nadc 8 -v4 -s 5
nofft ac-roundtrip song.wav 8 -v4 -s 5
nofft ac-info song.nadc                          # ... format=nfa4 baskets
```

The fixed header is the NFA1 20-byte layout (format 3, `block_len` 0); there is
no refit. `step` is the whole quality-per-rate knob: a finer grid (`-s 1`) gives
101 baskets and near-`bits=5` quality, a coarse one (`-s 20`) gives 6 baskets at
a fraction of the rate.

Measured at `bits=8`, `step=5` (`./measure_acp2`, T8/T9):

| Material | NFA3 b/s | NFA4 b/s | NFA4-NFA3 | NFA4 SNR | NFA3 SNR |
| --- | --- | --- | --- | --- | --- |
| crafted tone->noise, 10 s | 6.1709 | 4.1608 | -2.010 | 25.00 dB | 36.44 dB |
| stationary noise, 10 s | 8.0067 | 4.3244 | -3.682 | 26.02 dB | 35.54 dB |
| stationary tone 440, 10 s | 6.8084 | 4.1230 | -2.685 | 27.31 dB | 61.39 dB |
| tones + noise, 10 s | 7.5691 | 4.1126 | -3.457 | 23.50 dB | 40.23 dB |
| real stereo track (orig.wav) | 5.2790 | 3.0458 | -2.233 | 17.19 dB | 31.22 dB |
| real stereo track (orig1.wav) | 4.3936 | 2.8034 | -1.590 | 16.14 dB | 25.46 dB |
| listen track, 10.4 M frames | 3.5443 | 3.0406 | **-0.504** | 17.16 dB | 27.91 dB |

Dropping the predictor costs a lot of fidelity but buys a lot of rate: at
`step=5` NFA4 runs 1.6-3.7 b/s below NFA3 on real material, at roughly the
quality of a low-bit NFA1. The `step` sweep on `orig.wav` (T10) makes the
trade explicit:

| step | baskets | NFA4 b/s | NFA4 SNR |
| ---: | ---: | --- | --- |
| 1 | 101 | 5.3345 | 31.27 dB |
| 2 | 51 | 4.3412 | 25.21 dB |
| 5 | 21 | 3.0458 | 17.19 dB |
| 10 | 11 | 2.1260 | 10.97 dB |
| 20 | 6 | 1.1955 | 6.17 dB |

The basket count is `floor(100/step) + 1`, and the per-sample symbol costs about
`log2(baskets)` bits, so `step` moves rate and fidelity together. This is a
deliberately simple, predictor-free codec: one histogram, one symbol per sample.


Speed pays for the second symbol. Decoding drops to about 3.1 M frames/s on the
crafted mono signal and 2.5 on the stereo track (NFA1: 16.2 and 9.3; NFA3: 6.8
and 4.2), i.e. roughly half NFA3's throughput; encoding is about 2x NFA3's,
because every sample now runs both the residual symbol and a multi-bin table
look-up.

### Limits worth knowing

- **Input range.** The header fields are 16-bit fixed point over a nominal
  [-1, 1] signal, so samples outside +/-2.0, or NaN, are rejected with
  `ERR_BAD_INPUT` rather than clamped into a stream that decodes to noise.
- **Quality saturates near 16 bits.** The step is stored in 16 bits, so once the
  quantiser wants to be finer than `1/32768` the header can no longer express it
  and extra widths buy nothing: on the test track bits 16, 20 and 24 all measure
  87.0 dB. The rate keeps climbing anyway, so there is no reason to ask for more
  than 16.
- **Steady tones saturate early.** A pure tone's best one-step gain is
  `2*cos(w)`, which is above 1 for anything below about 60 Hz at 48 kHz, so `a1`
  clamps and the noise gain `1/(1 - a1)` is large. A tone gains far less than
  6 dB per bit. Real material has broadband noise and behaves like the table.
- **Overshoot.** Reconstruction can exceed the input peak (1.010 at `bits=6` on
  the test track). Output is 32-bit float, so nothing clips, but converting to
  16-bit later will.
- **Rates are content dependent.** Every number above comes from one bass-heavy
  track whose energy is 78.7% below 1 kHz. Bright material will cost more.


## Library API

`nofft.h` is usable directly. Errors are returned as an `enum Err` and rendered
through the `g_err_str` function pointer, so you can localise or reformat them.

```c
pcm_buf p;
if (wav_load("song.wav", &p) != ERR_OK)
    return 1;

if (nofft_encode("song.no_fft", &p, 20, 4, NOFFT_COEF_F16,
                 NOFFT_FIT_LEAST_SQ) != ERR_OK)
    return 1;

pcm_free(&p);
```

Also exported: `wav_save`, `wav_write_stream`, `nofft_decode`,
`nofft_decode_snr_db`, `nofft_coeff_format_str` / `_parse`,
`nofft_fit_str` / `_parse`, `nofft_file_fit`, and the solvers
(`nofft_solve_vandermonde`, `nofft_solve_least_squares`, `nofft_eval_poly`) with
their helpers.

`nofft_encode` takes a `nofft_fit` as its last argument; `NOFFT_FIT_INTERP` is the
previous behaviour.

`acpcm.h` is the second codec, usable the same way: `acpcm_encode`,
`acpcm_decode`, `acpcm_file_info` and `acpcm_info`; `acpcm_encode2` and
`acpcm_p2_policy` add the experimental NFA2 refit policies, `acpcm_encode3`
the NFA3 residual basket codec, and `acpcm_encode4` the NFA4 basket-only codec
(which also takes the `step` bucket width).

The polynomial codec is C++ written in a C style: no classes, no STL containers,
no exceptions, manual allocation, `enum Err` returns. Buffers are freed with
`pcm_free`. `acpcm.cpp` uses `std::vector` internally, which is an implementation
detail; its public interface is the same `enum Err` style.

## Supported input

WAV only, read through dr_wav: PCM, IEEE float, A-law and mu-law. Output is
always 32-bit float WAV.

The `convert` command additionally reads anything `ffmpeg` can decode, by way of
a temporary 32-bit float WAV. `ffmpeg` is not a build or install dependency; only
that one command needs it at runtime.

## Verification

Built with `-Wall -Wextra -Werror -pedantic`. The f16 conversion is checked
exhaustively: all 65536 binary16 values round-trip, adjacent float values land
within 1 ulp, and exact ties round to even. ASan, UBSan and leak detection are
clean across degrees 0–63, frame lengths 1–200, both coefficient formats, mono /
stereo / float input, and the corrupt-file and error paths.

`make test` runs `nofft selftest`, which needs no input files: bit-exact
reconstruction of silence, constant DC and single samples, round trips at every
bit width from 2 to 16, a bounded-noise case, and the malformed input the
decoder has to reject (out-of-range samples, NaN, both ends of the bit-width
range, a truncated file, a foreign magic, and a `.nadc` offered to the
polynomial reader). 60 random bit-flip corruptions of a real `.nadc` produce no
sanitizer report and no crash, and truncation at every length down to 4 bytes is
an error rather than a short buffer.

The NFA2 refit machinery is covered by the same selftest: `ACP2_NEVER` must
decode bit-identical to NFA1 (reading boundary flags alone must not touch the
audio), silence and DC stay bit-exact under `ACP2_DRIFT` and `ACP2_ALWAYS`, a
nonstationary signal must hold its SNR within 2 dB of NFA1 under every policy,
and a `block_len` of 0 — in the argument or patched into a written header — is
rejected rather than misparsed.

NFA3 and NFA4 get the same treatment: NFA3 round-trips a nonstationary signal
at `bits=6` within 2 dB of NFA1's SNR, keeps silence and DC bit-exact, survives
loud spike transients through the raw-escape path (SNR >= 30 dB), and reports
`format=2, block=0`; NFA4 codes a basket grid whose finer step strictly beats a
coarser one on the same signal, round-trips silence and DC *exactly* (a flat
channel has `peak_q == 0`, so every sample lands in bucket 0 whose midpoint is
0), codes an off-centre channel with sparse dips, and rejects `step` out of
range and a zero `bits` before any file is written.

The C build is checked against the C++ one rather than only against itself:
`make check-ac` generates a stereo test signal, encodes it with both tools at
every width from 2 to 24, and requires the files to be byte identical, then
decodes with both and requires the WAVs to be byte identical too. Both are built
with `-Wall -Wextra -Werror -pedantic`, and the C build adds `-Wshadow
-Wpointer-arith -Wcast-qual -Wstrict-prototypes -Wmissing-prototypes` plus
`-ffp-contract=off`, because a fused multiply-add in the predictor would round
differently from the C++ build and desync the recursive filter for the rest of the
file. `acc selftest` covers exact silence, quality rising monotonically with bit
width, symbol range at every width, stereo interleaving, and the malformed inputs
the decoder must reject.

`convert` is checked for shell-injection safety with filenames containing spaces
and metacharacters, against a missing `ffmpeg`, and for temporary-file leaks
across every success and failure path. `nofft_encode` removes its output when it
fails part way, so a truncated file is never mistaken for a valid one. A
converted file is byte-identical to encoding the same decoded WAV directly.

## Layout

```
main.cpp        CLI, including the ffmpeg-backed convert command
nofft.cpp       WAV I/O, interpolation, solver, f16 conversion, container
nofft.h         public API and format constants
acpcm.cpp       range coder, DPCM analyser and encoder, NFA1 container
acpcm.h         public API for the acpcm codec
selftest.cpp    built-in checks behind `nofft selftest`
selftest.h      self-test entry point
measure_acp2.cpp  scratch harness: NFA1/2/3/4 rate, SNR, speed, meter tables (not in `make`)
acpcm_inst.cpp  copy of acpcm.cpp with refit counters, for measure_acp2 (not in `make`)
c/ac.h          public API for the C acpcm codec
c/main.c        `acc` CLI, including its self test
c/acpcm.c       DPCM analysis, encoder, streaming interleaved decoder, NFA1
c/rc.c          carryless range coder, encoder and decoder
c/wavio.c       RIFF/WAVE reader and 32-bit float writer
c/play.c        ALSA playback, straight from the streaming decoder
c/Makefile      standalone C build
Makefile        host / windows / deb targets
debian/         packaging: control, copyright, changelog, nofft.1, acc.1
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

The acpcm codec in `NFA1` is implemented twice, in C++ and in C, and `make
check-ac` keeps the two honest by comparing them byte for byte at every bit
width. The C++ copy remains the reference. `NFA2`, `NFA3`, and `NFA4` are
experimental and C++ only: neither the C port nor `make check-ac` covers
them.