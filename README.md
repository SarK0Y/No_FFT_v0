# No_FFT

A lossy audio codec family for `.no_fft` and `.nadc` files. No FFT is involved,
at any stage.

This README covers **NFA2** and the MP3 converter. For the other algorithms, see:

- [NFA1: sample-domain DPCM with a range coder](./docs/nfa1.md)
- [NFA3: residual baskets](./docs/nfa3.md)
- [NFA4: basket-only, no predictor](./docs/nfa4.md)
- [No_FFT: polynomial-fit codec](./docs/nofft.md)

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

## Convert from MP3 (all codecs)

`convert-all` decodes an MP3 once (via `ffmpeg`) and writes every codec in one
pass, named after the output prefix:

```sh
nofft convert-all song.mp3 out
# out.poly.no_fft   polynomial fit
# out.nfa1.nadc     sample-domain DPCM
# out.nfa2.nadc     block-refit predictor
# out.nfa3.nadc     residual baskets
# out.nfa4.nadc     basket-only
```

`convert` writes a single codec selected with `-algo`. The two commands share
their flags: `-bits N` sets the acpcm quantiser width (default 6); `-block N`
and `-policy never|always|drift` shape NFA2; `-s N` or `-b N` shape NFA4 (a
step percent or an exact basket count, e.g. 170). `-algo poly` keeps the
historical polynomial positional arguments.

```sh
nofft convert song.mp3 song.no_fft -algo poly 20 4 f16
nofft convert song.mp3 song.nfa3.nadc -algo nfa3 -bits 6
nofft convert-all song.mp3 out -b 170          # NFA4 with 170 baskets
```

Both need `ffmpeg` on `PATH`; the decoded float32 WAV is temporary and removed
on every path. `convert-all` reports one line per file plus a byte total, and
keeps going if one algorithm fails. The
[converter guide](./docs/converter.md) has the full flag reference; see the
[No_FFT codec notes](./docs/nofft.md) for what `convert` does and does not suit.

## NFA2: forward adaptive predictor (experimental)

NFA1 fits `a1` once per channel and holds it for the whole file. When the
material changes character — a quiet passage into percussion, a tone into noise
— that one compromise gain costs bits for the rest of the file. NFA2 lets the
encoder replace `a1` at fixed block boundaries and tell the decoder when it
did. It is enabled per encode with `-block N` (the refit window in frames) and
`-policy never|always|drift` (default `drift`; `-policy` needs `-block`):

```sh
nofft ac-roundtrip song.wav 6 -block 1024 -policy drift
nofft ac-encode song.wav song.nadc 6 -block 1024 -policy drift
nofft ac-roundtrip song.wav 6 -block 1024        # policy defaults to drift
nofft ac-info song.nadc                          # ... block=1024
nofft ac-decode song.nadc song.out.wav
nofft ac-play song.nadc | mpv -
```

The library API is `acpcm_encode2` with `acpcm_p2_policy`; the decoder needs
nothing new and reads both magics. `ac-info` and the encoder/roundtrip summary
echo `block=` and `policy=` so a stream's shape is visible without a hex
editor.

### Container

Magic `NFA2`, and the fixed header gains one field, so the length table starts
at 24 instead of 20:

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

Measured at `bits=6`, `block_len=1024` (`./measure_acp2`):

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

## Links:  

**Rolling guide of TAM:** [https://alg0z8n8its9lovely6tricks.blogspot.com/2024/08/tam-guide-of-features-smart-tricks.html](https://alg0z8n8its9lovely6tricks.blogspot.com/2024/08/tam-guide-of-features-smart-tricks.html)  
**TELEGRAM:** [https://t.me/+N\_TdOq7Ui2ZiOTM6](https://t.me/+N_TdOq7Ui2ZiOTM6) (Alg0Z).  
**ALG0Z RU:** [https://dzen.ru/alg0z](https://dzen.ru/alg0z)  
**ALG0Z EN:** [https://alg0z.blogspot.com](https://alg0z.blogspot.com)  
**ChangeLog:** [https://alg0z8n8its9lovely6tricks.blogspot.com/2023/09/tam-changelog.html](https://alg0z8n8its9lovely6tricks.blogspot.com/2023/09/tam-changelog.html)  
**FORUM:** [https://www.neowin.net/forum/topic/1430114-tam/](https://www.neowin.net/forum/topic/1430114-tam/)  
**E-MAIL:** [sark0y@protonmail.com](mailto:sark0y@protonmail.com)  
**GITHUB:** [https://github.com/SarK0Y/TAM\_RUSTy.git](https://github.com/SarK0Y/TAM_RUSTy.git)  
**YouTube:** [https://www.youtube.com/@evgeneyknyazhev968](https://www.youtube.com/@evgeneyknyazhev968)  
**Twitter\_X:** [https://x.com/SarK0Y8](https://x.com/SarK0Y8)  
Donations: [https://boosty.to/alg0z/donate](https://boosty.to/alg0z/donate) [https://zap-hosting.com/en/shop/donation/1f0c83845d810df04ca74e56238399f7/](https://zap-hosting.com/en/shop/donation/1f0c83845d810df04ca74e56238399f7/)  

# Project has been assisted w/ awesome OpenCode 🙂

# my the Best Wishes to You, Dear User 🙃
