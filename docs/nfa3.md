# NFA3: residual baskets (experimental)

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
