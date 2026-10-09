# No_FFT: polynomial-fit audio codec

A lossy audio codec for `.no_fft` files that fits a **polynomial** to short blocks of
a waveform. No FFT is involved, at any stage.

Each block of `frame_len` frames is summarised by the coefficients of a polynomial
that passes through a small number of chosen samples. The decoder evaluates that
polynomial to rebuild the block. Because a block is described by `degree + 1`
numbers instead of `frame_len` samples, the file is smaller than the PCM it came
from, and the size/fidelity trade is yours to tune with `frame_len` and `degree`.

This is an experiment in time-domain polynomial fitting, not a competitor to
lossless codecs. It is lossy by construction.

## Use

```
nofft encode   <in.wav>    <out.no_fft> [frame_len] [degree] [f32|f16] [interp|least-sq]
nofft decode   <in.no_fft> <out.wav>
nofft roundtrip <in.wav>              [frame_len] [degree] [f32|f16] [interp|least-sq]
nofft convert  <in.mp3> <out.no_fft>  [frame_len] [degree] [f32|f16] [interp|least-sq]
nofft play     <in.no_fft>
```

`roundtrip` is the thing to reach for while tuning: it encodes, decodes, and
prints the resulting SNR without touching the disk.

```sh
nofft roundtrip song.wav 20 4        # defaults: frame_len 20, degree 3, f32
nofft encode song.wav song.no_fft 20 4 f16
nfft convert song.mp3 song.no_fft 20 4 f16
nofft play song.no_fft | mpv -
```

### From MP3

`convert` takes anything `ffmpeg` can read, MP3 included, decodes it to float32
WAV in a temporary file, and encodes that. The intermediate WAV is removed on
every path, including failures. Filenames go straight to `ffmpeg` via `execvp`,
never through a shell, so spaces and shell metacharacters in names are safe.

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

The fill byte is the block's residual RMS on a log scale over `[-140, 0]` dBFS
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

Each bound is a signed log-magnitude byte over `[-60, 0]` dBFS with the sign in
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
