# NFA6: blocked NFA5 (per-block log step)

NFA5 is a multiplicative (log-domain) predictor with one **global** quantum
`err` per chunk. That single step is its weakness: an `err` coarse enough to
hold the rate on loud or fast material is many sample units wide near silence,
so a quiet passage collapses to a few tenths of a bit per sample with poor
waveform fidelity (see [nfa5.md](./nfa5.md), "The low-rate dial"). NFA6 fixes
the collapse the way NFA2 fixed the same problem for the additive predictor:
give the container blocks and let the quantum adapt at each boundary.

It is, in one line: **NFA5's symbol stream with NFA2's blocked container.**

## The predictor

The grid, the offset and the symbol are exactly NFA5's:

    u[n] = u[n-1] * e^(K[n]*err),   u = xq + 32769
    K[n] = round( (log(u[n]) - log(u[n-1])) / (err * log(e)) )

The only change is that `err` is not a chunk-wide constant. Block `b` covers
frames `[b*block_len, (b+1)*block_len)` on the shifted grid (frame 0 is the
seed, so block 0 codes `block_len - 1` symbols), and each block carries its own
`err`. `e` still travels once per chunk.

## The per-block err code

Each block's `err` is range-coded in **8 bits** as a log-spaced code over NFA5's
accepted window `[ACPCM5_ERR_MIN, ACPCM5_ERR_MAX]`:

    code = round(255 * log(err / ERR_MIN) / log(ERR_MAX / ERR_MIN))

Both sides map the code back through the same `err_from_code8`, so the double
the encoder built its step table `F` from is exactly the double the decoder
rebuilds it from - only the encoder ever rounds into a code. One byte per block
is the entire overhead of adaptation.

## Encoder-side rate control

The block's `err` is a **rate-control** choice: the encoder bisects `err` down
until the block's estimated `K` cost fits a per-block slice of the target rate
(`-set-rate`, default `ACPCM6_RATE_DEFAULT` = 2.6 bits/sample). The cost model
is `k_entropy_bits`, the Shannon entropy of the `K` histogram at the candidate
`err` (the same estimator NFA5's auto-dial uses). A block whose ratios are all
zero - silence, DC, a flat span - takes the coarsest `err`, which is free
because every `K` is exactly 0.

The decoder never sees the policy, only the byte, so the fit could be anything:
it is not trusted and not reproduced. This is the NFA2 discipline exactly -
adapt encoder-side, transmit the decision.

## Container and format

Magic `NFA6`, format code 5. The fixed header is NFA2's 24-byte layout (the
NFA1 20-byte header plus `uint32 block_len` at offset 20); `block_len` is always
nonzero for NFA6 and is checked on load. Each channel is one range-coded chunk:

    2 x 32 bits   e          IEEE-754 double, both halves range-coded
    16 bits       seed       first sample as int16 (frame 0 of block 0)
    per block:
      8 bits      err code   log-spaced over the NFA5 err window
      ...         K[n]       one signed symbol per frame in the block

`K` reuses the NFA1 symbol model unchanged (zero flag, unary magnitude run,
mantissa, sign). There is no predictor fit and no basket table.

The 8-bit code lives *inside* the chunk stream, not the fixed header, because it
changes per block and per channel.

## Determinism

`e^(K*err)` must be identical on both sides, so `pow` never runs in the sample
loop. After each err code is read (or written), both sides rebuild the same
fixed-point table `F[K] = round(e^(K*err) * 2^16)` from the shared doubles, and
the update is then pure 64-bit integer arithmetic, exactly as NFA5. The grid is
rebuilt once per block; the rebuild is cheap (a few hundred table rows for a
typical `err`) and only at block boundaries.

## Bounds and edge cases

- `block_len` must be `>= 1`; `0` is `ERR_RANGE`.
- `e` must be finite and `> 1`; `rate` must be finite and in `0..64`; anything
  else is `ERR_RANGE`.
- `frames == 1`: seed only, no symbols.
- A block with no ratios (a one-frame tail) or a flat span gets the coarsest
  `err`; its `K` are all 0 and it costs the one err byte.
- Lossy like NFA5: only silence, DC and a one-frame chunk reconstruct exactly.

## Measured, and an honest verdict

At `-block 1024 -set-rate 2.6` (mono 44.1 kHz unless noted), against the plain
NFA5 auto-dial at the same target:

    signal                      NFA5 -set-rate 2.6    NFA6 -block 1024 -set-rate 2.6
    quiet tone (0.125)          39.82 dB @ 2.78       39.76 dB @ 2.79
    loud tone (0.9)             32.21 dB @ 2.60       32.12 dB @ 2.61
    standard (ac-gen)           18.52 dB @ 2.52       18.33 dB @ 2.76
    mixed quiet+loud tone       37.20 dB @ 2.43       30.98 dB @ 2.70
    mixed quiet+loud sweep      21.70 dB @ 1.89       13.83 dB @ 2.63
    stereo tone+chirp           13.92 dB @ 2.52       13.47 dB @ 2.78

NFA6 fixes the quiet collapse (39.8 dB instead of the 8.8 dB a loud-tuned fixed
`err` gives), but on this battery **it is matched or beaten by the NFA5
auto-dial on every signal.** The reason is that NFA5's *global* `err` already
self-allocates budget: a quiet block absorbs a fine step almost for free because
its `K` are mostly 0, so the bits flow to where they matter. Forcing an **equal
per-block budget** instead overpays quiet blocks and starves hard ones; the
mixed sweep is worst (13.8 dB against 21.7 dB).

So the recommended single dial for low-rate log-domain coding is
`-v5 -set-rate X` (one global auto-dialled `err`, no new format). NFA6 is kept
as the mode that gives a *bounded per-block rate* - useful when the fit has to
be piecewise, for latency or streaming - and it round-trips cleanly, but it is
not the better quality/rate point on mixed material.

## CLI

```sh
nofft ac-encode    song.wav song.nadc 8 -v6 -block 1024 -set-rate 2.6 -snr
nofft ac-roundtrip song.wav 8 -v6 -block 1024 -set-rate 2.6
nofft ac-info      song.nadc                 # ... format=nfa6 (block 1024)
nofft convert      song.mp3 song.nadc -algo nfa6 -block 1024 -set-rate 2.6
nofft convert-all  song.mp3 out/             # includes nfa6
```

`-block` and `-set-rate` are encode-time and require `-v6` (or `-algo nfa6`);
`-set-e` is also accepted (`-v5` and `-v6`). `-set-err` is NFA5-only: NFA6 has no
global err to set. `-block` defaults to 1024 and `-set-rate` to 2.6.

## Notes

- NFA6 shares all of NFA5's theory; read [nfa5.md](./nfa5.md) for the predictor,
  the moved grid, the closed loop and the lossy argument.
- The estimator gap is the same one NFA5's auto-dial documents: on chirp-heavy
  material the zero-order `K` entropy over-estimates what the adaptive range
  coder spends, so the block lands below its rate slice. NFA6's *equal* slices
  make that gap visible as a quality loss rather than a global rate undershoot.
- Encoder cost is a few times a plain NFA5 encode, dominated by the per-block
  bisection running the entropy estimator (about 3.0 s against 1.0 s for a 60 s
  stereo file here, and a smaller block barely changes it - the table rebuild is
  not the bottleneck). Decoding is unchanged.
- A per-block refit *without* the equal budgets - a budget pool that carries
  unspent bits across blocks - is the natural next experiment. It would push
  NFA6 toward the auto-dial's allocation while keeping the per-block cap.
