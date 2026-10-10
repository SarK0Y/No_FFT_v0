# NFA4: basket-only, no predictor (experimental)

NFA3 asks "is the mantissa-shaped unary run worth replacing?" NFA4 goes the
other way and asks how far one adaptive histogram can get *without* any
predictor or residual at all. Every sample carries exactly one symbol: its
basket index. There is no `a1` filter, no seed sample, no residual.

A basket index alone is a magnitude class, so to avoid a sign symbol the channel
grid is shifted first. The header carries the channel minimum `base_q`; every
sample maps to `u = xq - base_q >= 0`, and the decoder adds `base_q` back. On
that shifted grid `u` is split into `nb` equal baskets of the channel range
`peak_q`, with `factor = nb - 1`:

    b = floor(u * factor / peak_q)

`nb` itself is stored in the header (16 bits), so the grid is exactly the
requested basket count, up to `ACPCM4_MAX_BUCKETS = 256`. It is not capped at
the 101 rows an integer-percent step allows, which is what makes a 170-basket
grid possible. Both sides keep an adaptive `int32` histogram of basket counts,
one entry per basket. It starts at zero and is bumped after every sample — the
encoder the basket it just coded, the decoder the basket it just decoded — so the
model is recalculated after each sample and no table is transmitted. The decoder
rebuilds the basket midpoint

    mid_b = (ceil(b*peak_q/factor) + ceil((b+1)*peak_q/factor)) / 2

and returns `(base_q + mid_b) / 32768`. A flat or silent channel has `peak_q
== 0`, so every sample lands in basket 0 whose midpoint is 0 and the output is
`base_q` exactly: silence and DC round-trip bit for bit.

Although the header keeps the `bits` byte for compatibility, the basket grid is
set entirely by the basket count; the residual width does not enter (T8 shows the
NFA4 columns flat down the `bits` ladder).

Enabled with `-v4`, and either `-s step` (a bucket width in percent of the
channel range, mapped to `nb = 100/step + 1`, so at most 101 baskets) or `-b N`
(an exact basket count, 1..256, needed above 101); both need `-v4` and default to
`-s 5`:

```sh
nofft ac-encode song.wav song.nadc 8 -v4 -s 5
nofft ac-encode song.wav song.nadc 8 -v4 -b 170     # 170 baskets
nofft ac-roundtrip song.wav 8 -v4 -b 170
nofft ac-info song.nadc                             # ... format=nfa4 baskets
```

The fixed header is the NFA1 20-byte layout (format 3, `block_len` 0); there is
no refit. The basket count is the whole quality-per-rate knob: a fine grid
(`-b 256`) is the fidelity ceiling, a coarse one (`-b 2`) is a fraction of the
rate.

Measured at `bits=8`, `step=5` (`./measure_acp2`, T8/T9):

| Material | NFA3 b/s | NFA4 b/s | NFA4-NFA3 | NFA4 SNR | NFA3 SNR |
| --- | --- | --- | --- | --- | --- |
| crafted tone->noise, 10 s | 6.1709 | 4.1590 | -2.012 | 25.00 dB | 36.44 dB |
| stationary noise, 10 s | 8.0067 | 4.3226 | -3.684 | 26.02 dB | 35.54 dB |
| stationary tone 440, 10 s | 6.8084 | 4.1212 | -2.687 | 27.31 dB | 61.39 dB |
| tones + noise, 10 s | 7.5691 | 4.1108 | -3.458 | 23.50 dB | 40.23 dB |
| real stereo track (orig.wav) | 5.2790 | 3.0259 | -2.253 | 17.19 dB | 31.22 dB |
| real stereo track (orig1.wav) | 4.3936 | 2.7601 | -1.634 | 16.14 dB | 25.46 dB |
| listen track, 10.4 M frames | 3.5443 | 3.0199 | **-0.524** | 17.16 dB | 27.91 dB |

Dropping the predictor costs a lot of fidelity but buys a lot of rate: at 21
baskets NFA4 runs 1.6-3.7 b/s below NFA3 on most material (0.5 on the listen
track), at roughly the quality of a low-bit NFA1. The basket-count sweep on
`orig.wav` (T10) makes the trade explicit:

| baskets | NFA4 b/s | NFA4 SNR |
| ---: | --- | --- |
| 2 | 0.0000 | -0.00 dB |
| 6 | 1.1825 | 6.17 dB |
| 11 | 2.1122 | 10.97 dB |
| 21 | 3.0259 | 17.19 dB |
| 51 | 4.3149 | 25.21 dB |
| 101 | 5.3045 | 31.27 dB |
| 170 | 6.0562 | 35.90 dB |
| 256 | 6.6479 | 39.46 dB |

The per-sample symbol costs about `log2(baskets)` bits, so the count moves rate
and fidelity together. Pushing past the old 101-row cap keeps paying off — 170
baskets buys ~4.6 dB over 101 for ~0.75 b/s — which is why the count is stored
directly instead of a step percent. This is a deliberately simple,
predictor-free codec: one histogram, one symbol per sample.

Speed pays for the basket symbol. Decoding drops to about 4.0 M frames/s on the
crafted mono signal and 1.9 on the stereo track (NFA1: 18.0 and 10.8; NFA3: 7.3
and 4.3): each sample is one symbol read against a multi-bin histogram. Encoding
is *faster* than NFA3 (about 1.4x), because there is no predictor and no
residual symbol, just the one basket index.
