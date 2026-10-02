#define DR_WAV_IMPLEMENTATION
#include "third_party/dr_wav.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

enum Err {
    ERR_OK = 0,
    ERR_IO,
    ERR_OPEN,
    ERR_NOT_WAV,
    ERR_UNSUPPORTED_FMT,
    ERR_NO_MEMORY,
    ERR_COUNT
};

typedef const char* (*err_str_fn)(Err);

static const char* err_str_impl(Err e)
{
    switch (e) {
    case ERR_OK:              return "ok";
    case ERR_IO:              return "i/o error";
    case ERR_OPEN:            return "cannot open";
    case ERR_NOT_WAV:         return "not a wav file";
    case ERR_UNSUPPORTED_FMT: return "unsupported sample format";
    case ERR_NO_MEMORY:       return "out of memory";
    default:                  return "unknown error";
    }
}

static err_str_fn g_err_str = err_str_impl;

typedef struct {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint16_t format_tag;
    uint64_t frames;
} wav_info;

/* First 4 bytes of every container dr_wav understands. Mirrors the magic
   checks in drwav_init_file_ex() so we never reject a file it could read. */
static int wav_is_container(const unsigned char* magic)
{
    static const char* const known[] = { "RIFF", "RIFX", "riff", "RF64", "FORM" };

    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); ++i) {
        if (memcmp(magic, known[i], 4) == 0)
            return 1;
    }
    return 0;
}

static Err wav_probe(const char* path, wav_info* out)
{
    drwav wav;
    unsigned char magic[4];
    FILE* f;

    memset(out, 0, sizeof(*out));

    /* Open first: distinguishes "cannot reach the file" from "file is not audio". */
    f = fopen(path, "rb");
    if (f == nullptr)
        return ERR_OPEN;

    if (fread(magic, 1, sizeof(magic), f) != sizeof(magic)) {
        fclose(f);
        return ERR_NOT_WAV;
    }
    fclose(f);

    if (!wav_is_container(magic))
        return ERR_NOT_WAV;

    if (!drwav_init_file(&wav, path, nullptr))
        return ERR_NOT_WAV;

    switch (wav.translatedFormatTag) {
    case DR_WAVE_FORMAT_PCM:
    case DR_WAVE_FORMAT_IEEE_FLOAT:
    case DR_WAVE_FORMAT_ALAW:
    case DR_WAVE_FORMAT_MULAW:
        break;
    default:
        drwav_uninit(&wav);
        return ERR_UNSUPPORTED_FMT;
    }

    out->sample_rate     = wav.sampleRate;
    out->channels        = wav.channels;
    out->bits_per_sample = wav.bitsPerSample;
    out->format_tag      = wav.translatedFormatTag;
    out->frames          = wav.totalPCMFrameCount;

    drwav_uninit(&wav);
    return ERR_OK;
}

int main(int argc, char** argv)
{
    drwav_uint32 major = 0, minor = 0, rev = 0;
    drwav_version(&major, &minor, &rev);
    printf("dr_wav %u.%u.%u  |  ERR_COUNT=%d\n", major, minor, rev, (int)ERR_COUNT);

    for (int i = 1; i < argc; ++i) {
        wav_info info;
        Err e = wav_probe(argv[i], &info);

        if (e != ERR_OK) {
            printf("%-24s %s\n", argv[i], g_err_str(e));
            continue;
        }

        printf("%-24s %u ch  %u Hz  %2u bit  tag=0x%04X  %llu frames  %.3f s\n",
               argv[i], info.channels, info.sample_rate, info.bits_per_sample,
               info.format_tag, (unsigned long long)info.frames,
               info.sample_rate ? (double)info.frames / info.sample_rate : 0.0);
    }
    return 0;
}