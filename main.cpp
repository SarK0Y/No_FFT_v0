#include "nofft.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char* argv0)
{
    fprintf(stderr,
        "usage:\n"
        "  %s encode <in.wav> <out.no.fft> [frame_len] [degree] [f32|f16]\n"
        "  %s decode <in.no.fft> <out.wav>\n"
        "  %s roundtrip <in.wav> [frame_len] [degree] [f32|f16]\n"
        "  %s play <in.no_fft>                 (WAV to stdout, for piping)\n",
        argv0, argv0, argv0, argv0);
}

static uint32_t parse_u32(const char* s, uint32_t dflt)
{
    char* end = NULL;
    unsigned long v;

    if (s == NULL || *s == '\0')
        return dflt;

    v = strtoul(s, &end, 10);
    if (end == s || *end != '\0' || v == 0 || v > 0xFFFFFFFFul)
        return dflt;

    return (uint32_t)v;
}

int main(int argc, char** argv)
{
    uint32_t frame_len;
    uint32_t degree;
    nofft_coeff_format cf = NOFFT_COEF_F32;
    pcm_buf pcm, dec;
    Err e;



    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }

    if (strcmp(argv[1], "encode") == 0) {
        if (argc < 4) {
            usage(argv[0]);
            return 2;
        }
        frame_len = parse_u32(argc > 4 ? argv[4] : NULL, 20);
        degree = parse_u32(argc > 5 ? argv[5] : NULL, 3);

        if (argc > 6 && argv[6][0] != '\0' &&
            !nofft_coeff_format_parse(argv[6], &cf)) {
            fprintf(stderr, "unknown coefficient format: %s (use f32 or f16)\n", argv[6]);
            return 2;
        }

        e = wav_load(argv[2], &pcm);
        if (e != ERR_OK) {
            fprintf(stderr, "%s: %s\n", argv[2], g_err_str(e));
            return 1;
        }

        e = nofft_encode(argv[3], &pcm, frame_len, (uint16_t)degree, cf);
        if (e != ERR_OK) {
            fprintf(stderr, "%s: %s\n", argv[3], g_err_str(e));
            pcm_free(&pcm);
            return 1;
        }

        fprintf(stderr, "%s -> %s  %u ch  %u Hz  frame_len=%u degree=%u coef=%s  %zu samples\n",
                argv[2], argv[3], pcm.channels, pcm.sample_rate,
                frame_len, degree, nofft_coeff_format_str(cf), pcm.count);

        pcm_free(&pcm);
        return 0;
    }

    if (strcmp(argv[1], "decode") == 0) {
        if (argc < 4) {
            usage(argv[0]);
            return 2;
        }

        e = nofft_decode(argv[2], &dec);
        if (e != ERR_OK) {
            fprintf(stderr, "%s: %s\n", argv[2], g_err_str(e));
            return 1;
        }

        e = wav_save(argv[3], &dec);
        if (e != ERR_OK) {
            fprintf(stderr, "%s: %s\n", argv[3], g_err_str(e));
            pcm_free(&dec);
            return 1;
        }

        fprintf(stderr, "%s -> %s  %u ch  %u Hz  %zu samples\n",
                argv[2], argv[3], dec.channels, dec.sample_rate, dec.count);

        pcm_free(&dec);
        return 0;
    }

    if (strcmp(argv[1], "play") == 0) {
        if (argc < 3) {
            usage(argv[0]);
            return 2;
        }

        e = nofft_decode(argv[2], &dec);
        if (e != ERR_OK) {
            fprintf(stderr, "%s: %s\n", argv[2], g_err_str(e));
            return 1;
        }

        e = wav_write_stream(stdout, &dec);
        if (e != ERR_OK) {
            fprintf(stderr, "%s: %s\n", argv[2], g_err_str(e));
            pcm_free(&dec);
            return 1;
        }

        pcm_free(&dec);
        return 0;
    }

    if (strcmp(argv[1], "roundtrip") == 0) {
        size_t n;
        float snr;

        if (argc < 2) {
            usage(argv[0]);
            return 2;
        }

        frame_len = parse_u32(argc > 3 ? argv[3] : NULL, 20);
        degree = parse_u32(argc > 4 ? argv[4] : NULL, 3);

        if (argc > 5 && argv[5][0] != '\0' &&
            !nofft_coeff_format_parse(argv[5], &cf)) {
            fprintf(stderr, "unknown coefficient format: %s (use f32 or f16)\n", argv[5]);
            return 2;
        }

        e = wav_load(argv[2], &pcm);
        if (e != ERR_OK) {
            fprintf(stderr, "%s: %s\n", argv[2], g_err_str(e));
            return 1;
        }

        e = nofft_encode("/tmp/opencode/rt.no.fft", &pcm, frame_len, (uint16_t)degree, cf);
        if (e != ERR_OK) {
            fprintf(stderr, "encode: %s\n", g_err_str(e));
            pcm_free(&pcm);
            return 1;
        }

        e = nofft_decode("/tmp/opencode/rt.no.fft", &dec);
        if (e != ERR_OK) {
            fprintf(stderr, "decode: %s\n", g_err_str(e));
            pcm_free(&pcm);
            return 1;
        }

        n = (pcm.count < dec.count) ? pcm.count : dec.count;
        snr = nofft_decode_snr_db(pcm.samples, dec.samples, n);

        fprintf(stderr, "frame_len=%-5u degree=%-3u coef=%-3s  in=%zu out=%zu  SNR=%.2f dB\n",
                frame_len, degree, nofft_coeff_format_str(cf), pcm.count, dec.count, (double)snr);

        pcm_free(&pcm);
        pcm_free(&dec);
        return 0;
    }

    usage(argv[0]);
    return 2;
}