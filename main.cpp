#include "nofft.h"
#include "acpcm.h"
#include "selftest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/stat.h>
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
        "  %s convert <in.mp3> <out> [-algo poly|nfa1|nfa2|nfa3|nfa4]\n"
        "               [frame_len] [degree] [f32|f16] [interp|least-sq]   (poly)\n"
        "               [-bits N] [-block N] [-policy ...] [-s N | -b N]   (nfa*)\n"
        "  %s convert-all <in.mp3> <out-prefix>\n"
        "               [frame_len] [degree] [f32|f16] [interp|least-sq]\n"
        "               [-bits N] [-block N] [-policy ...] [-s N | -b N]\n"
        "  %s play <in.no_fft>                 (WAV to stdout, for piping)\n"
        "\n"
        "acpcm, sample-domain DPCM with a range coder (no polynomial):\n"
        "  %s ac-encode <in.wav> <out.nadc> [bits] [-snr] [-block N] [-policy never|always|drift] [-v3] [-v4 -s N | -b N]\n"
        "  %s ac-decode <in.nadc> <out.wav>\n"
        "  %s ac-play <in.nadc>               (WAV to stdout, for piping)\n"
        "  %s ac-info <in.nadc>\n"
        "  %s ac-roundtrip <in.wav> [bits] [-block N] [-policy never|always|drift] [-v3] [-v4 -s N | -b N]\n"
        "  %s ac-gen <out.wav> [seconds] [ch] [rate]\n"
        "  %s ac-play-alsa <in.nadc> [-d device]\n"
        "  %s selftest\n"
        "    -block N       write NFA2, refit the predictor every N frames\n"
        "    -policy when   when to refit: never, always or drift (default;\n"
        "                   needs -block; NFA2 with never still costs flags)\n"
        "    -v3            write NFA3 (adaptive basket table; no -block)\n"
        "    -v4            write NFA4 (basket-only; no -block, no -v3)\n"
        "    -s N           NFA4 basket grid: N percent of the channel range\n"
        "                   (default 5; caps at 101 baskets; NFA4 ignores `bits`)\n"
        "    -b N           NFA4 basket count instead of a step percent, 1..256\n"
        "                   (needed above 101, e.g. -b 170; needs -v4)\n"
        "    -algo name     convert codec: poly (default), nfa1, nfa2, nfa3, nfa4\n"
        "    -bits N        acpcm quantiser width for convert/convert-all (default 6)\n",
        argv0, argv0, argv0, argv0, argv0, argv0,
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

/* Encode one destination with the named algorithm and print the same one-line
   summary that `convert` prints.  Returns ERR_OK, or the codec error to report.
   Shared by `convert` (one algo) and `convert-all` (every algo). */
static Err convert_emit(const char* algo, const char* in, const char* out,
                        const pcm_buf* pcm, uint32_t cbits,
                        uint32_t block_len, acpcm_p2_policy policy,
                        uint32_t frame_len, uint32_t degree,
                        nofft_coeff_format cf, nofft_fit fit,
                        uint32_t step,
                        bool have_baskets, uint32_t baskets)
{
    Err e;

    if (strcmp(algo, "poly") == 0) {
        e = nofft_encode(out, pcm, frame_len, (uint16_t)degree, cf, fit);
    } else if (strcmp(algo, "nfa1") == 0) {
        e = acpcm_encode(out, pcm, (uint16_t)cbits);
    } else if (strcmp(algo, "nfa2") == 0) {
        e = acpcm_encode2(out, pcm, (uint16_t)cbits, block_len, policy);
    } else if (strcmp(algo, "nfa3") == 0) {
        e = acpcm_encode3(out, pcm, (uint16_t)cbits);
    } else {
        e = have_baskets
                ? acpcm_encode4_baskets(out, pcm, (uint16_t)cbits, baskets)
                : acpcm_encode4(out, pcm, (uint16_t)cbits, step);
    }
    if (e != ERR_OK)
        return e;

    if (strcmp(algo, "poly") == 0) {
        fprintf(stderr,
                "%s -> %s  %u ch  %u Hz  frame_len=%u degree=%u coef=%s fit=%s  %zu samples\n",
                in, out, pcm->channels, pcm->sample_rate,
                frame_len, degree, nofft_coeff_format_str(cf),
                nofft_fit_str(fit), pcm->count);
    } else {
        acpcm_info info;
        double bps = 0.0;
        char extra[64];

        memset(&info, 0, sizeof(info));
        if (acpcm_file_info(out, &info) == ERR_OK && info.frames != 0)
            bps = 8.0 * (double)info.payload /
                  ((double)info.frames * (double)info.channels);
        extra[0] = '\0';
        if (strcmp(algo, "nfa4") == 0)
            snprintf(extra, sizeof(extra),
                     have_baskets ? "  baskets=%u" : "  step=%u",
                     have_baskets ? baskets : step);
        else if (strcmp(algo, "nfa2") == 0)
            snprintf(extra, sizeof(extra), "  block=%u policy=%s",
                     block_len, policy_str(policy));
        fprintf(stderr,
                "%s -> %s  %u ch  %u Hz  algo=%s bits=%u%s  %.4f b/sample  %llu bytes\n",
                in, out, pcm->channels, pcm->sample_rate, algo,
                cbits, extra, bps, (unsigned long long)info.payload);
    }
    return ERR_OK;
}

int main(int argc, char** argv)
{
    uint32_t frame_len = 20;
    uint32_t degree = 3;
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

        /* -algo selects the codec.  The polynomial codec keeps the historical
           positional [frame_len] [degree] [f32|f16] [interp|least-sq]; the
           acpcm algorithms take flags instead. */
        const char* algo = "poly";
        uint32_t cbits = 6;
        bool have_bits = false;
        uint32_t block_len = 0;
        acpcm_p2_policy policy = ACP2_DRIFT;
        bool have_block = false;
        bool have_policy = false;
        bool have_step = false;
        uint32_t step = 5;
        bool have_baskets = false;
        uint32_t baskets = 170;
        const char* pos[4] = { NULL, NULL, NULL, NULL };
        int npos = 0;

        for (int i = 4; i < argc; i++) {
            if (strcmp(argv[i], "-algo") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "-algo needs poly, nfa1, nfa2, nfa3 or nfa4\n");
                    return 2;
                }
                algo = argv[++i];
                continue;
            }
            if (strcmp(argv[i], "-bits") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "-bits needs a width\n");
                    return 2;
                }
                cbits = parse_u32(argv[++i], 0);
                have_bits = true;
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
                have_block = true;
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
            if (strcmp(argv[i], "-s") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "-s needs a basket width in percent (>= 1)\n");
                    return 2;
                }
                step = parse_u32(argv[++i], 0);
                if (step < ACPCM4_STEP_MIN || step > ACPCM4_STEP_MAX) {
                    fprintf(stderr, "-s must be %u..%u\n",
                            ACPCM4_STEP_MIN, ACPCM4_STEP_MAX);
                    return 2;
                }
                have_step = true;
                continue;
            }
            if (strcmp(argv[i], "-b") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "-b needs a basket count (1..%u)\n",
                            ACPCM4_MAX_BUCKETS);
                    return 2;
                }
                baskets = parse_u32(argv[++i], 0);
                if (baskets < ACPCM4_BASKETS_MIN ||
                    baskets > ACPCM4_MAX_BUCKETS) {
                    fprintf(stderr, "-b must be %u..%u\n",
                            ACPCM4_BASKETS_MIN, ACPCM4_MAX_BUCKETS);
                    return 2;
                }
                have_baskets = true;
                continue;
            }
            if (argv[i][0] == '-') {
                fprintf(stderr, "unknown flag: %s\n", argv[i]);
                return 2;
            }
            if (npos >= 4) {
                fprintf(stderr, "too many arguments\n");
                return 2;
            }
            pos[npos++] = argv[i];
        }

        int is_poly = (strcmp(algo, "poly") == 0);
        int is_nfa1 = (strcmp(algo, "nfa1") == 0);
        int is_nfa2 = (strcmp(algo, "nfa2") == 0);
        int is_nfa3 = (strcmp(algo, "nfa3") == 0);
        int is_nfa4 = (strcmp(algo, "nfa4") == 0);
        if (!is_poly && !is_nfa1 && !is_nfa2 && !is_nfa3 && !is_nfa4) {
            fprintf(stderr,
                    "unknown -algo: %s (use poly, nfa1, nfa2, nfa3 or nfa4)\n",
                    algo);
            return 2;
        }

        if (is_poly) {
            frame_len = parse_u32(pos[0], 20);
            degree = parse_u32(pos[1], 3);
            if (pos[2] != NULL && pos[2][0] != '\0' &&
                !nofft_coeff_format_parse(pos[2], &cf)) {
                fprintf(stderr,
                        "unknown coefficient format: %s (use f32 or f16)\n",
                        pos[2]);
                return 2;
            }
            if (!parse_fit(pos[3], &fit))
                return 2;
            if (have_block || have_policy || have_step || have_baskets ||
                have_bits) {
                fprintf(stderr,
                        "-bits/-block/-policy/-s/-b are only for the nfa* algos\n");
                return 2;
            }
        } else {
            if (npos != 0) {
                fprintf(stderr, "the nfa* algos take flags, not positional args\n");
                return 2;
            }
            if (cbits < ACPCM_BITS_MIN || cbits > ACPCM_BITS_MAX) {
                fprintf(stderr, "-bits must be %d..%d\n",
                        ACPCM_BITS_MIN, ACPCM_BITS_MAX);
                return 2;
            }
            if ((have_block || have_policy) && !is_nfa2) {
                fprintf(stderr, "-block/-policy are only for -algo nfa2\n");
                return 2;
            }
            if ((have_step || have_baskets) && !is_nfa4) {
                fprintf(stderr, "-s/-b are only for -algo nfa4\n");
                return 2;
            }
            if (have_step && have_baskets) {
                fprintf(stderr, "use either -s or -b, not both\n");
                return 2;
            }
            if (have_policy && !have_block) {
                fprintf(stderr, "-policy needs -block\n");
                return 2;
            }
            if (is_nfa2 && !have_block)
                block_len = 1024;   /* sensible refit window by default */
        }

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

        e = convert_emit(algo, argv[2], argv[3], &pcm, cbits, block_len,
                         policy, frame_len, degree, cf, fit,
                         step, have_baskets, baskets);
        if (e != ERR_OK) {
            fprintf(stderr, "%s: %s\n", argv[3], g_err_str(e));
            pcm_free(&pcm);
            remove(tmp);
            free(tmp);
            return 1;
        }

        pcm_free(&pcm);
        remove(tmp);
        free(tmp);
        return 0;
    }

    if (strcmp(argv[1], "convert-all") == 0) {
        static const char* const algos[] = { "poly", "nfa1", "nfa2",
                                             "nfa3", "nfa4" };
        const size_t n_algos = sizeof(algos) / sizeof(algos[0]);
        char* tmp;
        uint32_t cbits = 6;
        uint32_t block_len = 1024;
        acpcm_p2_policy policy = ACP2_DRIFT;
        bool have_step = false;
        uint32_t step = 5;
        bool have_baskets = false;
        uint32_t baskets = 170;
        const char* pos[4] = { NULL, NULL, NULL, NULL };
        int npos = 0;
        int failures = 0;
        uint64_t total_bytes = 0;

        if (argc < 4) {
            usage(argv[0]);
            return 2;
        }

        /* Same flags as `convert`, but every algorithm runs, so there is no
           -algo.  The polynomial positional arguments still apply to `poly`. */
        for (int i = 4; i < argc; i++) {
            if (strcmp(argv[i], "-bits") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "-bits needs a width\n");
                    return 2;
                }
                cbits = parse_u32(argv[++i], 0);
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
                i++;
                continue;
            }
            if (strcmp(argv[i], "-s") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "-s needs a basket width in percent (>= 1)\n");
                    return 2;
                }
                step = parse_u32(argv[++i], 0);
                if (step < ACPCM4_STEP_MIN || step > ACPCM4_STEP_MAX) {
                    fprintf(stderr, "-s must be %u..%u\n",
                            ACPCM4_STEP_MIN, ACPCM4_STEP_MAX);
                    return 2;
                }
                have_step = true;
                continue;
            }
            if (strcmp(argv[i], "-b") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "-b needs a basket count (1..%u)\n",
                            ACPCM4_MAX_BUCKETS);
                    return 2;
                }
                baskets = parse_u32(argv[++i], 0);
                if (baskets < ACPCM4_BASKETS_MIN ||
                    baskets > ACPCM4_MAX_BUCKETS) {
                    fprintf(stderr, "-b must be %u..%u\n",
                            ACPCM4_BASKETS_MIN, ACPCM4_MAX_BUCKETS);
                    return 2;
                }
                have_baskets = true;
                continue;
            }
            if (argv[i][0] == '-') {
                fprintf(stderr, "unknown flag: %s\n", argv[i]);
                return 2;
            }
            if (npos >= 4) {
                fprintf(stderr, "too many arguments\n");
                return 2;
            }
            pos[npos++] = argv[i];
        }

        frame_len = parse_u32(pos[0], 20);
        degree = parse_u32(pos[1], 3);
        if (pos[2] != NULL && pos[2][0] != '\0' &&
            !nofft_coeff_format_parse(pos[2], &cf)) {
            fprintf(stderr, "unknown coefficient format: %s (use f32 or f16)\n",
                    pos[2]);
            return 2;
        }
        if (!parse_fit(pos[3], &fit))
            return 2;

        if (cbits < ACPCM_BITS_MIN || cbits > ACPCM_BITS_MAX) {
            fprintf(stderr, "-bits must be %d..%d\n",
                    ACPCM_BITS_MIN, ACPCM_BITS_MAX);
            return 2;
        }
        if (have_step && have_baskets) {
            fprintf(stderr, "use either -s or -b, not both\n");
            return 2;
        }

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

        for (size_t k = 0; k < n_algos; k++) {
            char out[4096];
            struct stat st;
            const char* ext = strcmp(algos[k], "poly") == 0 ? "no_fft"
                                                            : "nadc";
            int n = snprintf(out, sizeof(out), "%s.%s.%s",
                             argv[3], algos[k], ext);

            if (n < 0 || (size_t)n >= sizeof(out)) {
                fprintf(stderr, "%s.%s.%s: output path too long\n",
                        argv[3], algos[k], ext);
                failures++;
                continue;
            }

            e = convert_emit(algos[k], argv[2], out, &pcm, cbits, block_len,
                             policy, frame_len, degree, cf, fit,
                             step, have_baskets, baskets);
            if (e != ERR_OK) {
                fprintf(stderr, "%s: %s\n", out, g_err_str(e));
                failures++;
                continue;
            }
            if (stat(out, &st) == 0)
                total_bytes += (uint64_t)st.st_size;
        }

        fprintf(stderr,
                "convert-all: %zu codecs, %d failed, %llu bytes total\n",
                n_algos, failures, (unsigned long long)total_bytes);

        pcm_free(&pcm);
        remove(tmp);
        free(tmp);
        return failures == 0 ? 0 : 1;
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
        bool v3 = false;
        bool v4 = false;
        bool have_step = false;
        uint32_t step = 5;
        bool have_baskets = false;
        uint32_t baskets = 170;
        bool show_snr = false;
        const char* in = NULL;
        const char* out = NULL;

        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-snr") == 0) {
                show_snr = true;
                continue;
            }
            if (strcmp(argv[i], "-v3") == 0) {
                v3 = true;
                continue;
            }
            if (strcmp(argv[i], "-v4") == 0) {
                v4 = true;
                continue;
            }
            if (strcmp(argv[i], "-s") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "-s needs a basket width in percent (>= 1)\n");
                    return 2;
                }
                step = parse_u32(argv[++i], 0);
                if (step < ACPCM4_STEP_MIN || step > ACPCM4_STEP_MAX) {
                    fprintf(stderr, "-s must be %u..%u\n",
                            ACPCM4_STEP_MIN, ACPCM4_STEP_MAX);
                    return 2;
                }
                have_step = true;
                continue;
            }
            if (strcmp(argv[i], "-b") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "-b needs a basket count (1..%u)\n",
                            ACPCM4_MAX_BUCKETS);
                    return 2;
                }
                baskets = parse_u32(argv[++i], 0);
                if (baskets < ACPCM4_BASKETS_MIN ||
                    baskets > ACPCM4_MAX_BUCKETS) {
                    fprintf(stderr, "-b must be %u..%u\n",
                            ACPCM4_BASKETS_MIN, ACPCM4_MAX_BUCKETS);
                    return 2;
                }
                have_baskets = true;
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
        if (v3 && (block_len != 0 || have_policy)) {
            fprintf(stderr, "-v3 cannot be combined with -block or -policy\n");
            return 2;
        }
        if (v4 && (block_len != 0 || have_policy || v3)) {
            fprintf(stderr, "-v4 cannot be combined with -block, -policy or -v3\n");
            return 2;
        }
        if (have_step && !v4) {
            fprintf(stderr, "-s needs -v4\n");
            return 2;
        }
        if (have_baskets && !v4) {
            fprintf(stderr, "-b needs -v4\n");
            return 2;
        }
        if (have_step && have_baskets) {
            fprintf(stderr, "use either -s or -b, not both\n");
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

        if (v4)
            e = have_baskets
                    ? acpcm_encode4_baskets(out, &pcm, (uint16_t)bits, baskets)
                    : acpcm_encode4(out, &pcm, (uint16_t)bits, step);
        else if (v3)
            e = acpcm_encode3(out, &pcm, (uint16_t)bits);
        else if (block_len != 0)
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
            if (v4)
                snprintf(extra, sizeof(extra),
                         have_baskets ? "  baskets=%u" : "  step=%u",
                         have_baskets ? baskets : step);
            else if (v3)
                snprintf(extra, sizeof(extra), "  baskets");
            else if (info.block_len != 0)
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
            const char* fname = (info.format == 3) ? "nfa4"
                              : (info.format == 2) ? "nfa3"
                              : (info.format == 1) ? "nfa2" : "nfa1";
            if (info.format == 3)
                snprintf(extra, sizeof(extra), "  format=%s baskets", fname);
            else if (info.format == 2)
                snprintf(extra, sizeof(extra), "  format=%s baskets", fname);
            else if (info.block_len != 0)
                snprintf(extra, sizeof(extra), "  format=%s block=%u", fname,
                         info.block_len);
            else
                snprintf(extra, sizeof(extra), "  format=%s", fname);
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
        bool v3 = false;
        bool v4 = false;
        bool have_step = false;
        uint32_t step = 5;
        bool have_baskets = false;
        uint32_t baskets = 170;
        double peak = 0.0;
        const char* in = NULL;

        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-v3") == 0) {
                v3 = true;
                continue;
            }
            if (strcmp(argv[i], "-v4") == 0) {
                v4 = true;
                continue;
            }
            if (strcmp(argv[i], "-s") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "-s needs a basket width in percent (>= 1)\n");
                    return 2;
                }
                step = parse_u32(argv[++i], 0);
                if (step < ACPCM4_STEP_MIN || step > ACPCM4_STEP_MAX) {
                    fprintf(stderr, "-s must be %u..%u\n",
                            ACPCM4_STEP_MIN, ACPCM4_STEP_MAX);
                    return 2;
                }
                have_step = true;
                continue;
            }
            if (strcmp(argv[i], "-b") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "-b needs a basket count (1..%u)\n",
                            ACPCM4_MAX_BUCKETS);
                    return 2;
                }
                baskets = parse_u32(argv[++i], 0);
                if (baskets < ACPCM4_BASKETS_MIN ||
                    baskets > ACPCM4_MAX_BUCKETS) {
                    fprintf(stderr, "-b must be %u..%u\n",
                            ACPCM4_BASKETS_MIN, ACPCM4_MAX_BUCKETS);
                    return 2;
                }
                have_baskets = true;
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
        if (v3 && (block_len != 0 || have_policy)) {
            fprintf(stderr, "-v3 cannot be combined with -block or -policy\n");
            return 2;
        }
        if (v4 && (block_len != 0 || have_policy || v3)) {
            fprintf(stderr, "-v4 cannot be combined with -block, -policy or -v3\n");
            return 2;
        }
        if (have_step && !v4) {
            fprintf(stderr, "-s needs -v4\n");
            return 2;
        }
        if (have_baskets && !v4) {
            fprintf(stderr, "-b needs -v4\n");
            return 2;
        }
        if (have_step && have_baskets) {
            fprintf(stderr, "use either -s or -b, not both\n");
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

        if (v4)
            e = have_baskets
                    ? acpcm_encode4_baskets(tmp, &pcm, (uint16_t)bits, baskets)
                    : acpcm_encode4(tmp, &pcm, (uint16_t)bits, step);
        else if (v3)
            e = acpcm_encode3(tmp, &pcm, (uint16_t)bits);
        else if (block_len != 0)
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
                if (v4)
                    snprintf(extra, sizeof(extra),
                             have_baskets ? "  baskets=%u" : "  step=%u",
                             have_baskets ? baskets : step);
                else if (v3)
                    snprintf(extra, sizeof(extra), "  baskets");
                else if (block_len != 0)
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