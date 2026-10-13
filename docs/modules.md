# Loadable codec modules

The project can expose every codec as a *codec registry* that can be discovered and
used without directly naming the implementations. Two things make this possible:

- A **core library** (`libnofft.so` / `libnofft.a`) that contains the codec
  implementations, the registry, and the dynamic loader. It implements the
  buffer-based API (`*_buf` variants) so codecs never need to touch files.
- A set of **loadable modules** (`nofft_codec_*.so`) — one per codec (`poly`,
  `nfa1`, `nfa2`, `nfa3`, `nfa4`, `nfa5`, `nfa6`). Each is a tiny shim that
  exports `nofft_codec_plugin(const nofft_codec**)` and hands back the same
  `nofft_codec` vtable that is already compiled into the core library. The host
  can discover them at run time via `dlopen`/`LoadLibrary`.

Headers:

- `codec.h` — C ABI for the registry, plugin entry point, options and helpers
- `nofft.h`, `acpcm.h` — existing APIs (now with in-memory buffer variants)

## Using the registry in C/C++

Link against `libnofft` and include `codec.h`. A host can either use the
builtins directly, or load modules from disk.

```c
#include "codec.h"

int main(void) {
    /* autoloads builtins, then modules from NOFFT_MODULE_PATH */
    nofft_codec_autoload();

    const nofft_codec* c = nofft_codec_find("nfa3");
    if (c == NULL) return 1;

    pcm_buf pcm = ...;
    nofft_codec_opts opts; nofft_codec_opts_default(&opts); opts.bits = 6;

    uint8_t* buf = NULL; size_t len = 0;
    if (c->encode(&pcm, &opts, &buf, &len) != ERR_OK) return 1;
    /* ... use buf ... */
    nofft_codec_free(buf);
    pcm_free(&pcm);
    return 0;
}
```

### Loading modules explicitly

- `int nofft_codec_load(const char* path)` — load one `.so`/`.dll` and register
  whatever it exports
- `int nofft_codec_load_dir(const char* dir)` — load every module in a directory
- `void nofft_codec_set_search_path(const char* dirs)` — set the search path
  (directories separated by `:` on POSIX, `;` on Windows)
- `int nofft_codec_autoload(void)` — register builtins, then load modules from
  search path or `NOFFT_MODULE_PATH` (if no search path was set). Set
  `NOFFT_NO_BUILTINS=1` to register only loadable modules (useful when
  distributing a slim host).

Builtins are always accessible via `nofft_codec_poly()`, `nofft_codec_nfa1()`
.. `nofft_codec_nfa6()`, and `nofft_codec_find()` will look in the registry
first, then fall back to builtins.

### In-memory API

The module layer is buffer-based: `nofft_encode_buf`, `nofft_decode_buf`,
`nofft_buf_free`, and `acpcm_encode*_buf`, `acpcm_decode_buf`,
`acpcm_file_info_buf`, `acpcm_buf_free`. The original file-path APIs remain and
are thin wrappers; every encode path shares a single memory core so bytes are
identical.

## Building

```sh
make modules        # build libnofft.so/a, modules, and the demo tool
make test-modules   # exercise the registry, loadable modules, roundtrips
make install        # install headers, library, modules under PREFIX
make windows-modules # cross-build for win64 (needs mingw-w64)
```

## Demo tool

`build/bin/nofft_codec` demonstrates using the registry:

```sh
nofft_codec list                # list all codecs found
nofft_codec roundtrip nfa3 in.wav bits=6
nofft_codec encode poly in.wav out.no_fft frame=20 degree=3 coef=f32 fit=interp
nofft_codec decode nfa1 in.nadc out.wav
nofft_codec info - in.nadc      # try every registered codec

NOFFT_MODULE_PATH=/path/to/modules nofft_codec count
NOFFT_NO_BUILTINS=1 NOFFT_MODULE_PATH=/path/to/modules nofft_codec count
```

The modules are loadable but self-contained — each just delegates to the core's
vtable. A host that only ships the library can ignore them; a host that wants to
discover them at run time simply points `NOFFT_MODULE_PATH` at the `modules/`
directory.
