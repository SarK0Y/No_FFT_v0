# The converter: one input, every codec

`nofft` can turn anything `ffmpeg` reads — MP3 included — into any of its codec
formats. There are two entry points:

- `convert` writes **one** codec, chosen with `-algo`.
- `convert-all` decodes the input **once** and writes **all** of them.

Neither uses a shell: the input filename is handed to `ffmpeg` through `execvp`,
so spaces and shell metacharacters are safe. The decoded audio lands in a
temporary float32 WAV that is removed on every path, success or failure.

## Requirements

`ffmpeg` must be on `PATH`. Only these two commands need it; the rest of `nofft`
is self-contained.

## Quick start

```sh
nofft convert-all song.mp3 out
```

writes:

```
out.poly.no_fft   polynomial fit
out.nfa1.nadc     sample-domain DPCM
out.nfa2.nadc     block-refit predictor
out.nfa3.nadc     residual baskets
out.nfa4.nadc     basket-only
```

and prints one summary line per file followed by a byte total.

## The codecs

| `-algo` | Codec | Magic | Extension | Details |
| --- | --- | --- | --- | --- |
| `poly` (default) | polynomial fit | `NOFF` | `.no_fft` | [nofft.md](./nofft.md) |
| `nfa1` | DPCM + range coder | `NFA1` | `.nadc` | [nfa1.md](./nfa1.md) |
| `nfa2` | block-refit predictor | `NFA2` | `.nadc` | [nfa2.md](./nfa2.md) |
| `nfa3` | residual baskets | `NFA3` | `.nadc` | [nfa3.md](./nfa3.md) |
| `nfa4` | basket-only | `NFA4` | `.nadc` | [nfa4.md](./nfa4.md) |

`convert-all` names its files `<prefix>.<algo>.<ext>`, for example
`out.nfa3.nadc`. `convert` writes to the exact path you give it. Existing
output files are overwritten.

## `convert`: one codec

```
nofft convert <in> <out> [-algo poly|nfa1|nfa2|nfa3|nfa4]
             [frame_len] [degree] [f32|f16] [interp|least-sq]   (poly)
             [-bits N] [-block N] [-policy ...] [-s N | -b N]   (nfa*)
```

`-algo` defaults to `poly`, and the polynomial codec keeps its historical
positional arguments, so an existing command line keeps working:

```sh
nofft convert song.mp3 song.no_fft 20 4 f16          # poly, frame_len 20, degree 4, f16
nofft convert song.mp3 song.nfa1.nadc -algo nfa1 -bits 6
nofft convert song.mp3 song.nfa2.nadc -algo nfa2 -block 1024 -policy drift
nofft convert song.mp3 song.nfa3.nadc -algo nfa3 -bits 6
nofft convert song.mp3 song.nfa4.nadc -algo nfa4 -b 170
```

The `nfa*` algorithms take flags, not the polynomial positional arguments; a
bare number after `<out>` is rejected for them.

## `convert-all`: every codec

```
nofft convert-all <in> <out-prefix>
                 [frame_len] [degree] [f32|f16] [interp|least-sq]
                 [-bits N] [-block N] [-policy ...] [-s N | -b N]
```

It takes the same flags as `convert`, minus `-algo`, and writes all five files
in one decode. A failure on one algorithm is reported and the rest still run;
the exit status is non-zero if any failed.

```sh
nofft convert-all song.mp3 out
nofft convert-all song.mp3 out -b 170               # NFA4 with 170 baskets
nofft convert-all song.mp3 out 32 5 f16 least-sq -block 512 -policy always
```

`<out-prefix>` is a prefix, not a directory: `out` yields `out.poly.no_fft`,
`out.nfa1.nadc`, and so on.

## Flags

| Flag | Applies to | Meaning |
| --- | --- | --- |
| `-algo name` | `convert` | `poly` (default), `nfa1`, `nfa2`, `nfa3` or `nfa4` |
| `-bits N` | `nfa*` | quantiser width, 2..24 (default 6) |
| `-block N` | `nfa2` | refit window in frames, >= 1 (default 1024) |
| `-policy p` | `nfa2` | `never`, `always` or `drift` (default `drift`) |
| `-s N` | `nfa4` | basket grid as a step percent (default 5); above 100 the grid collapses to a single basket |
| `-b N` | `nfa4` | exact basket count, 1..256 (for example 170); mutually exclusive with `-s` |
| `[frame_len]` | `poly` | frames per block (default 20) |
| `[degree]` | `poly` | polynomial degree (default 3) |
| `[f32\|f16]` | `poly` | coefficient format (default f32) |
| `[interp\|least-sq]` | `poly` | fit (default interp) |

In `convert`, `-policy` needs `-block`, and flags aimed at an algorithm you did
not select are rejected. In `convert-all` NFA2 always runs, so `-block` defaults
to 1024 and `-policy` may be given on its own.

## What to expect

Conversion is mechanical and correct, but the family is lossy and MP3 is not the
material the polynomial codec likes: its reconstruction can be noisier than the
source on broadband content. See
[nofft.md](./nofft.md#what-mp3-cannot-be-represented-by). The acpcm algorithms
(`nfa1` through `nfa4`) generally do far better and are the ones to reach for.
As with any lossy step, check before converting an archive.

## Exit status

`0` on success, `1` on any error, `2` on a usage error. For `convert-all`, `1`
means at least one algorithm failed.
