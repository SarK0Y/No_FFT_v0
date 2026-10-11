# NFA5: multiplicative (log-domain) predictor

NFA1-4 all rest on an **additive** predictor: a sample is a linear function of
the previous one plus a quantised residual, so the coder spends bits on the
*difference* between neighbours and every sample carries the same absolute
error. NFA5 asks the opposite question. If the prediction is **multiplicative**,
the coder spends bits on the *ratio* between neighbours:

    S[n] = S[n-1] * e^k[n]

which is a companding / log-domain predictor. Equal absolute steps near silence
become equal **relative** steps everywhere, which suits the wide dynamic range
of audio in the same way a floating-point mantissa does. The catch is that the
ratio is a real number, so it must be quantised, and the format is therefore
**lossy** (see below).

## The predictor

The residual is the natural logarithm of the one-step ratio, quantised to
multiples of a global step `err`:

    k[n] = log( S[n] / S[n-1] ) = K[n] * err,   K integer
    S[n] = S[n-1] * e^k[n]

`K` is the only symbol per sample. `e` (base) and `err` (log-domain quantum) are
stored in the file so the decoder can rebuild the same table, and both are
chosen at encode time with `-set-e` / `-set-err`.

The two are not independent. Because

    e^(K*err) = (e^err)^K

only the per-step ratio `e^err` reaches the bitstream; `e` and `err` are a
convenience pair (a base and a tolerance), not two separate knobs. Setting
`e = 2` and a small `err` is the natural way to describe it.

## The moved grid

`log` needs a positive argument, and a multiplicative update through zero is
absorbing, so the channel is shifted onto a strictly positive grid:

    u[n] = S[n] + 32769,   S in [-32768, 32767]  =>  u in [1, 65536]

The offset is `32769`, not `32768`, precisely so no sample lands on `u = 0`
(where the ratio is undefined and the recurrence could never leave). `S` is the
usual 1/32768 quantised sample, so `u` is an `int32`. The recurrence runs on `u`:

    K[n] = round( (log(u[n]) - log(u[n-1])) / (err * log(e)) )
    u[n] = clamp( round(u[n-1] * e^(K[n]*err)), 1, 65536 )

and the decoder returns `S[n] = u[n] - 32769`.

## Closed loop

As in every DPCM codec here, the encoder must advance on the **reconstructed**
value, never on the source sample, or the two sides diverge on the first
rounding and never resynchronise:

    pred = clamp( round(prev * e^(K*err)), 1, 65536 )
    code K
    prev = pred

Only `K` is transmitted; the source `u[n]` is used exactly once, to choose `K`.
Because `K` is chosen against the reconstructed `prev`, each step's log error
`log(u[n]) - log(source[n])` is bounded by roughly `err/2` and does not random
walk across the channel.

## Determinism

`e^(K*err)` must be identical on both sides, so `pow` never runs in the sample
loop. Each side precomputes one fixed-point table per chunk,

    F[K] = round( e^(K*err) * 2^16 ),    K in [-KMAX, KMAX]

from the same decoded `e` and `err`, after which the update is pure integer
arithmetic in 64 bits:

    pred = (prev * F[K] + (1 << 15)) >> 16

then clamped. Both sides build `F` from the same doubles with the same code
path, so there is no ulp-level drift to resynchronise. `KMAX` spans the whole
positive grid (a ratio of `65536`):

    KMAX = ceil( log(65536) / err ) + 1,   clamped to ACPCM5_MAX_STEPS (32768)

A factor is also clipped at `2^34`, well inside the 64-bit product; anything
that large clamps the update to the grid top anyway.

## Bounds and edge cases

- `err` is accepted in `ACPCM5_ERR_MIN .. ACPCM5_ERR_MAX` (`5e-4 .. 1.0`) and
  `e` must be finite and `> 1`; anything else is `ERR_RANGE`. The floor on
  `err` is what keeps `KMAX` inside the table cap, so the full grid stays
  reachable.
- A `|K|` beyond `KMAX` is **clamped**, not escaped: still a valid prediction,
  just coarser. The decoder clamps too, so the two sides never disagree on a
  corrupt symbol.
- `frames == 1`: seed only, no symbols.
- A constant channel (`u[n] == u[n-1]`) gives `K = 0`, so silence and DC are the
  cheapest possible stream and reconstruct exactly (`F[0] = 2^16`).

## Lossy, not lossless

Unlike NFA1-4, NFA5 **cannot** round-trip bit for bit. `e^(K*err)` lands on a
geometric grid, and `round(prev * e^(K*err))` will not hit every integer; that is
the whole point of relative-precision coding. So:

- `test_nfa5` asserts bounds (a minimum SNR, monotonicity in `err`) rather than
  equality. Silence, DC and a one-frame chunk are the only exact cases.
- `convert` treats NFA5 as a lossy target; it is a size/quality experiment, not
  an archival format.
- The existing lossless guarantees of NFA1-4 are untouched.

The measured curve on a 440 Hz tone (mono, `e = 2`) sits next to NFA1:

    err       SNR       rate
    0.02     39.4 dB   5.84 b/sample
    0.01     45.5 dB   6.84 b/sample
    0.001    65.5 dB  10.20 b/sample

## Format

Container magic `NFA5`; the fixed header is the NFA1 20-byte layout (magic,
`frames`, `bits`, `channels`, `sample_rate`, then `len[c]` per channel). Each
channel is its own range-coded chunk that begins with its own coded fields, so
`e` and `err` travel with the stream instead of being compile-time constants:

    2 x 32 bits   e        IEEE-754 double, both halves range-coded
    2 x 32 bits   err      IEEE-754 double, both halves range-coded
    16 bits       seed     first sample as int16 (the raw 1/32768 value)
    ...           K[n]     one signed symbol per frame after the seed

`bits` is stored in the header but is only a label here; the grid is set
entirely by `e` and `err`. `K` reuses the NFA1 symbol model unchanged (zero
flag, unary magnitude run, mantissa bits, sign). There is no predictor fit, no
refit blocks, and no basket table: the entire model is the single `F` table and
one adaptive symbol model.

## CLI

```sh
nofft ac-encode    song.wav song.nadc 8 -v5 -set-e 2 -set-err 0.01 -snr
nofft ac-roundtrip song.wav 8 -v5 -set-e 2 -set-err 0.01
nofft ac-info      song.nadc                 # ... format=nfa5 log-grid
nofft convert      song.mp3 song.nadc -algo nfa5 -set-e 2 -set-err 0.01
nofft convert-all  song.mp3 out/             # includes nfa5
```

`-set-e` and `-set-err` are encode-time and require `-v5` (or `-algo nfa5`); the
decoder reads both from the file. They cannot be combined with `-block`,
`-policy`, `-v3`, `-v4`, `-s` or `-b`.

## Notes

- `e` and `err` live in each chunk rather than the fixed header, so a
  random-access reader only needs the length table; a chunk is self-describing.
- Default `err` is `0.01`; `bits` does not influence it.
- A first-order context for `K` (the previous `K`, as NFA3 does for baskets) is
  an obvious next experiment, since consecutive log-ratios are strongly
  correlated on smooth signals. It is not implemented.
