# Vendored: No_FFT codecs

This directory is a **verbatim, unmodified** copy of the codec sources from

https://github.com/SarK0Y/No_FFT_v0.git

pinned to the `main` branch (release mode).

The NFM container is not present on the current upstream `main` branch.
When absent, `nfm.cpp` and `nfm.h` are fetched from the compatibility
branch `trunk`; the codec sources below remain from `main`.

| Field | Value |
| --- | --- |
| Repo | `https://github.com/SarK0Y/No_FFT_v0.git` |
| Mode | `release` |
| Branch | `main` |
| Commit | `91318adb03da11560671a5e18eae15cdbc60404c` |
| Date | `2026-10-24T12:50:53+03:00` |
| NFM compatibility | `trunk` when absent from `main` |

## Vendored files

| File | Origin |
| --- | --- |
| `nofft.cpp`, `nofft.h` | polynomial-fit codec (`.no_fft`) from `main` |
| `acpcm.cpp`, `acpcm.h` | DPCM/basket codecs (`.nadc`, NFA1–NFA6) from `main` |
| `nfm.cpp`, `nfm.h` | NFM container (metadata and cover art), compatibility branch `trunk` if absent |
| `third_party/dr_wav.h` | upstream WAV reader dependency |
| `LICENSE` | upstream licence |

Nothing here is edited by Nova: all glue lives in `crates/nova-codec`
(`shim/nova_nofft_shim.cpp` gives the C++ code a stable C ABI) and
`crates/nova-codec/build.rs`.

## Refreshing

NOVA_SYNC_NOFFT=1 cargo build                       # refresh default branch main
NOFFT_MODE=dev NOVA_SYNC_NOFFT=1 cargo build         # refresh dev branch trunk
NOFFT_BRANCH=my-branch NOVA_SYNC_NOFFT=1 cargo build # explicit branch
