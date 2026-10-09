# NFA1: sample-domain DPCM with a range coder

The `ac-*` commands are a second, independent codec: it keeps **no polynomial at
all**, and is usually the better choice. Files use the `.nadc` extension —
**N**o-**F**FT **A**udio **D**PCM **C**odec — with the four byte magic `NFA1`
(**N**o **F**FT **A**udio codec, format revision 1), so the two formats never
collide and both readers stay simple.

```sh
nofft ac-roundtrip song.wav 6     # 30.7 dB at 3.03 bits per sample
nofft ac-encode song.wav song.nadc 6
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

## How it works

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

## Format

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
