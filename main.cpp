#include "nofft.h"
#include "acpcm.h"
#include "selftest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <cmath>

static double compute_snr(const float* a, const float* b, size_t n)
{
    double num = 0.0, den = 0.0;
    for (size_t i = 0; i < n; i++) {
        double e = (double)a[i] - (double)b[i];
        double s = (double)a[i];
        num += e*e;
        den += s*s;
    }
    if (den <= 0.0) return 999.0;
    if (num <= 0.0) return 999.0;
    return 10.0 * log10(den / num);
}
static void usage(const char* argv0)
{
    fprintf(stderr,
        "usage:\n"
        "  %s encode <in.wav> <out.no.fft> [frame_len] [degree] [f32|f16] [interp|least-sq]\n"
        "  %s decode <in.no.fft> <out.wav>\n"
        "  %s roundtrip <in.wav> [frame_len] [degree] [f32|f16] [interp|least-sq]\n"
        "  %s convert <in.mp3> <out.no_fft> [frame_len] [degree] [f32|f16] [interp|least-sq]\n"
        "  %s play <in.no_fft>                 (WAV to stdout, for piping)\n"
        "\n"
        "acpcm, sample-domain DPCM with a range coder (no polynomial):\n"
        "  %s ac-encode <in.wav> <out.nadc> [bits] [-snr] [-block N] [-policy never|always|drift]\n"
        "  %s ac-decode <in.nadc> <out.wav>\n"
        "  %s ac-play <in.nadc>               (WAV to stdout, for piping)\n"
        "  %s ac-info <in.nadc>\n"
        "  %s ac-roundtrip <in.wav> [bits] [-block N] [-policy never|always|drift]\n"
        "  %s ac-gen <out.wav> [seconds] [ch] [rate]\n"
        "  %s ac-play-alsa <in.nadc> [-d device]\n"
        "  %s selftest\n"
        "    -block N       write NFA2, refit the predictor every N frames\n"
        "    -policy when   when to refit: never, always or drift (default;\n"
        "                   needs -block; NFA2 with never still costs flags)\n",
        argv0, argv0, argv0, argv0, argv0,
        argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0);
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

/* Parses an NFA2 refit policy. Returns 0 on a bad value. */
static int parse_policy(const char* s, acpcm_p2_policy* out)
{
    if (s == NULL || *s == '\0')
        return 0;
    if (strcmp(s, "never") == 0) {
        *out = ACP2_NEVER;
        return 1;
    }
    if (strcmp(s, "always") == 0) {
        *out = ACP2_ALWAYS;
        return 1;
    }
    if (strcmp(s, "drift") == 0) {
        *out = ACP2_DRIFT;
        return 1;
    }
    return 0;
}

static const char* policy_str(acpcm_p2_policy p)
{
    if (p == ACP2_ALWAYS)
        return "always";
    if (p == ACP2_DRIFT)
        return "drift";
    return "never";
}

/* Parses the optional trailing fit selector. Returns 0 on a bad value. */
static int parse_fit(const char* s, nofft_fit* out)
{
    if (s == NULL || s[0] == '\0')
        return 1;
    if (nofft_fit_parse(s, out))
        return 1;
    fprintf(stderr, "unknown fit: %s (use interp or least-sq)\n", s);
    return 0;
}

/* Run ffmpeg to decode src (mp3 or anything ffmpeg reads) into a float32 wav
   at dst. execvp is used directly so no filename ever passes through a shell. */
static int run_ffmpeg(const char* src, const char* dst)
{
    const char* argv[16];
    pid_t pid;
    int status = 0;

    argv[0] = "ffmpeg";
    argv[1] = "-nostdin";
    argv[2] = "-hide_banner";
    argv[3] = "-loglevel";
    argv[4] = "error";
    argv[5] = "-y";
    argv[6] = "-i";
    argv[7] = src;
    argv[8] = "-f";
    argv[9] = "wav";
    argv[10] = "-c:a";
    argv[11] = "pcm_f32le";
    argv[12] = dst;
    argv[13] = NULL;

    pid = fork();
    if (pid < 0)
        return -1;

    if (pid == 0) {
        execvp(argv[0], (char* const*)argv);
        _exit(127);
    }

    if (waitpid(pid, &status, 0) < 0)
        return -1;

    if (!WIFEXITED(status))
        return -1;
    if (WEXITSTATUS(status) == 127) {
        fprintf(stderr, "ffmpeg not found in PATH (install it to use convert)\n");
        return -1;
    }
    if (WEXITSTATUS(status) != 0)
        return -1;

    return 0;
}

/* scratch wav path for roundtrip/convert, removed by the caller */
static char* temp_wav_path(void)
{
    const char* tmpdir = getenv("TMPDIR");
    char* p;
    size_t n;

    if (tmpdir == NULL || *tmpdir == '\0')
        tmpdir = "/tmp";

    n = strlen(tmpdir) + 32;
    p = (char*)malloc(n);
    if (p == NULL)
        return NULL;

    snprintf(p, n, "%s/nofft_XXXXXX.wav", tmpdir);

    if (mkstemps(p, 4) == -1) {
        free(p);
        return NULL;
    }

    return p;
}

int main(int argc, char** argv)
{
    uint32_t frame_len;
    uint32_t degree;
    nofft_coeff_format cf = NOFFT_COEF_F32;
    nofft_fit fit = NOFFT_FIT_INTERP;
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

        if (!parse_fit(argc > 7 ? argv[7] : NULL, &fit))
            return 2;

        e = wav_load(argv[2], &pcm);
        if (e != ERR_OK) {
            fprintf(stderr, "%s: %s\n", argv[2], g_err_str(e));
            return 1;
        }

        e = nofft_encode(argv[3], &pcm, frame_len, (uint16_t)degree, cf, fit);
        if (e != ERR_OK) {
            fprintf(stderr, "%s: %s\n", argv[3], g_err_str(e));
            pcm_free(&pcm);
            return 1;
        }

        fprintf(stderr,
                "%s -> %s  %u ch  %u Hz  frame_len=%u degree=%u coef=%s fit=%s  %zu samples\n",
                argv[2], argv[3], pcm.channels, pcm.sample_rate,
                frame_len, degree, nofft_coeff_format_str(cf),
                nofft_fit_str(fit), pcm.count);

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
        char* tmp;
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

        if (!parse_fit(argc > 6 ? argv[6] : NULL, &fit))
            return 2;

        e = wav_load(argv[2], &pcm);
        if (e != ERR_OK) {
            fprintf(stderr, "%s: %s\n", argv[2], g_err_str(e));
            return 1;
        }

        tmp = temp_wav_path();
        if (tmp == NULL) {
            fprintf(stderr, "cannot create a temporary file\n");
            pcm_free(&pcm);
            return 1;
        }

        e = nofft_encode(tmp, &pcm, frame_len, (uint16_t)degree, cf, fit);
        if (e != ERR_OK) {
            fprintf(stderr, "encode: %s\n", g_err_str(e));
            pcm_free(&pcm);
            remove(tmp);
            free(tmp);
            return 1;
        }

        e = nofft_decode(tmp, &dec);
        if (e != ERR_OK) {
            fprintf(stderr, "decode: %s\n", g_err_str(e));
            pcm_free(&pcm);
            free(tmp);
            return 1;
        }

        remove(tmp);
        free(tmp);

        n = (pcm.count < dec.count) ? pcm.count : dec.count;
        snr = nofft_decode_snr_db(pcm.samples, dec.samples, n);

        fprintf(stderr, "frame_len=%-5u degree=%-3u coef=%-3s fit=%-8s  in=%zu out=%zu  SNR=%.2f dB\n",
                frame_len, degree, nofft_coeff_format_str(cf), nofft_fit_str(fit),
                pcm.count, dec.count, (double)snr);

        pcm_free(&pcm);
        pcm_free(&dec);
        return 0;
    }

    if (strcmp(argv[1], "convert") == 0) {
        char* tmp;

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

        if (!parse_fit(argc > 7 ? argv[7] : NULL, &fit))
            return 2;

        tmp = temp_wav_path();
        if (tmp == NULL) {
            fprintf(stderr, "cannot create a temporary file\n");
            return 1;
        }

        if (run_ffmpeg(argv[2], tmp) != 0) {
            fprintf(stderr, "%s: cannot decode with ffmpeg\n", argv[2]);
            remove(tmp);
            free(tmp);
            return 1;
        }

        e = wav_load(tmp, &pcm);
        if (e != ERR_OK) {
            fprintf(stderr, "%s: decoded audio: %s\n", argv[2], g_err_str(e));
            remove(tmp);
            free(tmp);
            return 1;
        }

        e = nofft_encode(argv[3], &pcm, frame_len, (uint16_t)degree, cf, fit);
        if (e != ERR_OK) {
            fprintf(stderr, "%s: %s\n", argv[3], g_err_str(e));
            pcm_free(&pcm);
            remove(tmp);
            free(tmp);
            return 1;
        }

        fprintf(stderr,
                "%s -> %s  %u ch  %u Hz  frame_len=%u degree=%u coef=%s fit=%s  %zu samples\n",
                argv[2], argv[3], pcm.channels, pcm.sample_rate,
                frame_len, degree, nofft_coeff_format_str(cf),
                nofft_fit_str(fit), pcm.count);

        pcm_free(&pcm);
        remove(tmp);
        free(tmp);
        return 0;
    }

    if (strcmp(argv[1], "selftest") == 0)
        return acpcm_selftest(1) == ERR_OK ? 0 : 1;

    /* ---- acpcm ---- */

    if (strcmp(argv[1], "ac-encode") == 0) {
        acpcm_info info;
        uint32_t bits = 6;
        uint32_t block_len = 0;
        acpcm_p2_policy policy = ACP2_DRIFT;
        bool have_policy = false;
        bool show_snr = false;
        const char* in = NULL;
        const char* out = NULL;

        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-snr") == 0) {
                show_snr = true;
                continue;
            }
            if (strcmp(argv[i], "-block") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "-block needs a frame count (>= 1)\n");
                    return 2;
                }
                block_len = parse_u32(argv[++i], 0);
                if (block_len == 0) {
                    fprintf(stderr, "-block must be >= 1\n");
                    return 2;
                }
                continue;
            }
            if (strcmp(argv[i], "-policy") == 0) {
                if (i + 1 >= argc || !parse_policy(argv[i + 1], &policy)) {
                    fprintf(stderr, "policy must be never, always or drift\n");
                    return 2;
                }
                have_policy = true;
                i++;
                continue;
            }
            if (argv[i][0] == '-') {
                fprintf(stderr, "unknown flag: %s\n", argv[i]);
                return 2;
            }
            if (in == NULL) { in = argv[i]; continue; }
            if (out == NULL) { out = argv[i]; continue; }
            bits = parse_u32(argv[i], 6);
        }
        if (in == NULL || out == NULL) {
            usage(argv[0]);
            return 2;
        }
        if (have_policy && block_len == 0) {
            fprintf(stderr, "-policy needs -block\n");
            return 2;
        }
        if (bits < ACPCM_BITS_MIN || bits > ACPCM_BITS_MAX) {
            fprintf(stderr, "bits must be %d..%d\n", ACPCM_BITS_MIN, ACPCM_BITS_MAX);
            return 2;
        }

        e = wav_load(in, &pcm);
        if (e != ERR_OK) {
            fprintf(stderr, "%s: %s\n", in, g_err_str(e));
            return 1;
        }

        if (block_len != 0)
            e = acpcm_encode2(out, &pcm, (uint16_t)bits, block_len, policy);
        else
            e = acpcm_encode(out, &pcm, (uint16_t)bits);
        if (e != ERR_OK) {
            fprintf(stderr, "%s: %s\n", out, g_err_str(e));
            pcm_free(&pcm);
            return 1;
        }

        if (acpcm_file_info(out, &info) == ERR_OK && info.frames != 0) {
            double bps = 8.0 * (double)info.payload /
                         ((double)info.frames * (double)info.channels);
            char extra[64];
            extra[0] = '\0';
            if (info.block_len != 0)
                snprintf(extra, sizeof(extra), "  block=%u policy=%s",
                         info.block_len, policy_str(policy));
            fprintf(stderr,
                    "%s -> %s  %u ch  %u Hz  bits=%u%s  %.4f b/sample  %llu bytes\n",
                    in, out, pcm.channels, pcm.sample_rate, bits, extra, bps,
                    (unsigned long long)info.payload);
        }

        if (show_snr) {
            pcm_buf decb;
            memset(&decb, 0, sizeof(decb));
            e = acpcm_decode(out, &decb);
            if (e == ERR_OK && decb.count == pcm.count) {
                double snr = compute_snr(pcm.samples, decb.samples, pcm.count);
                fprintf(stderr, "SNR: %.2f dB\n", snr);
            }
            pcm_free(&decb);
        }

        pcm_free(&pcm);
        return 0;
    }
    if (strcmp(argv[1], "ac-decode") == 0) {
        if (argc < 4) {
            usage(argv[0]);
            return 2;
        }

        e = acpcm_decode(argv[2], &dec);
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

    if (strcmp(argv[1], "ac-gen") == 0) {
        if (argc < 3) {
            usage(argv[0]);
            return 2;
        }
        double secs = (argc >= 4) ? atof(argv[3]) : 0.5;
        uint32_t ch = (argc >= 5) ? (uint32_t)atoi(argv[4]) : 2;
        uint32_t rate = (argc >= 6) ? (uint32_t)atoi(argv[5]) : 44100;
        Err e = acpcm_gen(argv[2], secs, ch, rate);
        if (e != ERR_OK) {
            fprintf(stderr, "%s: %s\n", argv[2], g_err_str(e));
            return 1;
        }
        return 0;
    }
    if (strcmp(argv[1], "ac-play") == 0) {
        if (argc < 3) {
            usage(argv[0]);
            return 2;
        }

        e = acpcm_decode(argv[2], &dec);
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

    if (strcmp(argv[1], "ac-play-alsa") == 0 || strcmp(argv[1], "ac-play-device") == 0) {
        if (argc < 3) {
            usage(argv[0]);
            return 2;
        }
        const char* device = NULL;
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
                device = argv[++i];
        }
        Err e = acpcm_play(argv[2], device);
        if (e != ERR_OK) {
            fprintf(stderr, "%s: %s\n", argv[2], g_err_str(e));
            return 1;
        }
        return 0;
    }
    if (strcmp(argv[1], "ac-info") == 0) {
        acpcm_info info;

        if (argc < 3) {
            usage(argv[0]);
            return 2;
        }

        e = acpcm_file_info(argv[2], &info);
        if (e != ERR_OK) {
            fprintf(stderr, "%s: %s\n", argv[2], g_err_str(e));
            return 1;
        }

        if (info.frames != 0) {
            double bps = 8.0 * (double)info.payload /
                         ((double)info.frames * (double)info.channels);
            char extra[32];
            extra[0] = '\0';
            if (info.block_len != 0)
                snprintf(extra, sizeof(extra), "  block=%u", info.block_len);
            fprintf(stderr,
                    "%s  bits=%u  %u ch  %u Hz  %llu frames  %.4f b/sample  %llu bytes%s\n",
                    argv[2], info.bits, info.channels, info.sample_rate,
                    (unsigned long long)info.frames, bps,
                    (unsigned long long)info.payload, extra);
        }
        return 0;
    }

    if (strcmp(argv[1], "ac-roundtrip") == 0) {
        char* tmp;
        size_t n;
        float snr;
        uint32_t bits = 6;
        uint32_t block_len = 0;
        acpcm_p2_policy policy = ACP2_DRIFT;
        bool have_policy = false;
        double peak = 0.0;
        const char* in = NULL;

        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-block") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "-block needs a frame count (>= 1)\n");
                    return 2;
                }
                block_len = parse_u32(argv[++i], 0);
                if (block_len == 0) {
                    fprintf(stderr, "-block must be >= 1\n");
                    return 2;
                }
                continue;
            }
            if (strcmp(argv[i], "-policy") == 0) {
                if (i + 1 >= argc || !parse_policy(argv[i + 1], &policy)) {
                    fprintf(stderr, "policy must be never, always or drift\n");
                    return 2;
                }
                have_policy = true;
                i++;
                continue;
            }
            if (argv[i][0] == '-') {
                fprintf(stderr, "unknown flag: %s\n", argv[i]);
                return 2;
            }
            if (in == NULL) { in = argv[i]; continue; }
            bits = parse_u32(argv[i], 6);
        }
        if (in == NULL) {
            usage(argv[0]);
            return 2;
        }
        if (have_policy && block_len == 0) {
            fprintf(stderr, "-policy needs -block\n");
            return 2;
        }
        if (bits < ACPCM_BITS_MIN || bits > ACPCM_BITS_MAX) {
            fprintf(stderr, "bits must be %d..%d\n", ACPCM_BITS_MIN, ACPCM_BITS_MAX);
            return 2;
        }

        e = wav_load(in, &pcm);
        if (e != ERR_OK) {
            fprintf(stderr, "%s: %s\n", in, g_err_str(e));
            return 1;
        }

        tmp = temp_wav_path();
        if (tmp == NULL) {
            fprintf(stderr, "cannot create a temporary file\n");
            pcm_free(&pcm);
            return 1;
        }

        if (block_len != 0)
            e = acpcm_encode2(tmp, &pcm, (uint16_t)bits, block_len, policy);
        else
            e = acpcm_encode(tmp, &pcm, (uint16_t)bits);
        if (e != ERR_OK) {
            fprintf(stderr, "encode: %s\n", g_err_str(e));
            pcm_free(&pcm);
            remove(tmp);
            free(tmp);
            return 1;
        }

        {
            acpcm_info finfo;
            double bps = -1.0;
            if (acpcm_file_info(tmp, &finfo) == ERR_OK && finfo.frames != 0)
                bps = 8.0 * (double)finfo.payload /
                      ((double)finfo.frames * (double)finfo.channels);

            e = acpcm_decode(tmp, &dec);
            if (e != ERR_OK) {
                fprintf(stderr, "decode: %s\n", g_err_str(e));
                pcm_free(&pcm);
                remove(tmp);
                free(tmp);
                return 1;
            }

            remove(tmp);
            free(tmp);

            n = (pcm.count < dec.count) ? pcm.count : dec.count;
            snr = nofft_decode_snr_db(pcm.samples, dec.samples, n);

            for (size_t i = 0; i < dec.count; i++) {
                double a = dec.samples[i] < 0.0f ? -(double)dec.samples[i]
                                                 : (double)dec.samples[i];
                if (a > peak)
                    peak = a;
            }

            {
                char extra[64];
                extra[0] = '\0';
                if (block_len != 0)
                    snprintf(extra, sizeof(extra), "  block=%u policy=%s",
                             block_len, policy_str(policy));
                fprintf(stderr,
                        "bits=%-3u in=%zu out=%zu%s  SNR=%.2f dB  %.4f b/sample"
                        "  peak=%.4f%s\n",
                        bits, pcm.count, dec.count, extra, (double)snr,
                        bps >= 0.0 ? bps : 0.0, peak,
                        peak > 1.0 ? "  (overshoots full scale)" : "");
            }
        }

        pcm_free(&pcm);
        pcm_free(&dec);
        return 0;
    }

    usage(argv[0]);
    return 2;
}