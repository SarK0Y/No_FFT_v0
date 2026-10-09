# NFA4: basket-only, no predictor (experimental)

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
