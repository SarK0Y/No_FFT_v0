#ifndef SELFTEST_H
#define SELFTEST_H

#include "nofft.h"

/* Runs the acpcm checks that do not need any input files: exact reconstruction
   of signals that must survive untouched, round trips across the bit widths,
   and the malformed input the decoder has to reject.  Returns ERR_OK when
   everything passes.  `verbose` prints one line per case. */
Err acpcm_selftest(int verbose);

#endif