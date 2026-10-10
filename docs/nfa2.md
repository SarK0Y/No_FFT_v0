# NFA2: forward adaptive predictor (experimental)

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
