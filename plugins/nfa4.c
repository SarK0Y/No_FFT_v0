/* nfa4.c - the loadable module for the "nfa4" codec.
 *
 * A module is deliberately thin: it hands the registry the same vtable the
 * core library already carries (nofft_codec_nfa4), so a host can discover and
 * load codecs at run time without linking a copy of every implementation into
 * the plugin.  The whole module ABI is codec.h; the symbol below is the only
 * thing the loader looks for.
 */

#include "codec.h"

#if defined(_WIN32)
#define NOFFT_EXPORT __declspec(dllexport)
#else
#define NOFFT_EXPORT __attribute__((visibility("default")))
#endif

NOFFT_EXPORT int nofft_codec_plugin(const nofft_codec** out)
{
    if (out == NULL)
        return -1;
    *out = nofft_codec_nfa4();
    return (*out == NULL) ? -1 : 0;
}
