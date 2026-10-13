#include "selftest.h"
#include "acpcm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Deterministic noise so the results do not depend on a rand implementation. */
static uint32_t rng_state;

static void rng_seed(uint32_t s)
{
    rng_state = s ? s : 1;
}

static float rng_next(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return ((float)(int32_t)rng_state / 2147483648.0f) * 0.5f;
}

static int failures;

static void check(int ok, int verbose, const char* what, const char* detail)
{
    if (!ok)
        failures++;
    if (verbose || !ok)
        printf("  %-4s %-34s %s\n", ok ? "ok" : "FAIL", what, detail ? detail : "");
}

static void temp_path(char* buf, size_t n, const char* stem)
{
    const char* tmp = getenv("TMPDIR");
    if (tmp == NULL || *tmp == '\0')
        tmp = "/tmp";
    snprintf(buf, n, "%s/nofft_selftest_%s.bin", tmp, stem);
}

/* exact means every sample survives bit for bit */
static int same(const pcm_buf* a, const pcm_buf* b)
{
    if (a->count != b->count || a->channels != b->channels ||
        a->sample_rate != b->sample_rate)
        return 0;
    for (size_t i = 0; i < a->count; i++) {
        float x = a->samples[i];
        float y = b->samples[i];
        if (x != y && !(x != x && y != y))
            return 0;
    }
    return 1;
}

/* encode then decode through the real file path, so the container is covered
   too and not just the codec in memory */
static Err trip(const pcm_buf* in, uint16_t bits, pcm_buf* out)
{
    char path[512];
    Err e;

    temp_path(path, sizeof(path), "rt");
    e = acpcm_encode(path, in, bits);
    if (e != ERR_OK)
        return e;
    e = acpcm_decode(path, out);
    remove(path);
    return e;
}

/* Same, NFA1 or NFA2, and reports the payload rate in bits per sample. */
static Err trip_x(const pcm_buf* in, uint16_t bits, int v2, uint32_t block_len,
                  acpcm_p2_policy pol, pcm_buf* out, double* bps)
{
    char path[512];
    Err e;

    temp_path(path, sizeof(path), "drift");
    e = v2 ? acpcm_encode2(path, in, bits, block_len, pol)
           : acpcm_encode(path, in, bits);
    if (e == ERR_OK) {
        if (bps != NULL) {
            acpcm_info info;
            memset(&info, 0, sizeof(info));
            if (acpcm_file_info(path, &info) == ERR_OK && info.frames != 0)
                *bps = 8.0 * (double)info.payload /
                       ((double)info.frames * (double)info.channels);
        }
        e = acpcm_decode(path, out);
    }
    remove(path);
    return e;
}

/* Signals that carry no information the quantiser must throw away, plus the
   two cases that cannot be bit exact: a two frame signal, and a steady tone
   whose best one step predictor sits right on the stability clamp. */
static void test_exact(int verbose)
{
    struct {
        const char* name;
        size_t frames;
        uint16_t ch;
        int kind;       /* 0 zero, 1 dc, 2 one sample, 3 two samples, 4 tone */
        double min_snr; /* 999 means the samples must match bit for bit */
    } cases[] = {
        { "silence is bit exact",      4096, 2, 0, 999.0 },
        { "constant dc is bit exact",  4096, 1, 1, 999.0 },
        { "single sample survives",       1, 1, 2, 999.0 },
        { "zero signal survives",      1024, 6, 0, 999.0 },
        { "two samples survive",          2, 1, 3,  60.0 },
        /* a1 clamps to 0.999 and 1/(1-a1) is then the noise gain, so a tone
           saturates instead of gaining 6 dB per bit */
        { "steady tone stays bounded",  4096, 1, 4,  20.0 },
    };

    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        pcm_buf in, out;
        char det[192];
        double snr = 0.0;
        int ok;

        memset(&in, 0, sizeof(in));
        in.count = cases[c].frames * cases[c].ch;
        in.channels = cases[c].ch;
        in.sample_rate = 44100;
        in.samples = (float*)calloc(in.count, sizeof(float));
        if (in.samples == NULL)
            return;

        for (size_t i = 0; i < in.count; i++) {
            if (cases[c].kind == 1)
                in.samples[i] = 0.75f;                    /* dc */
            else if (cases[c].kind == 2)
                in.samples[i] = 0.5f;
            else if (cases[c].kind == 3)
                in.samples[i] = (i == 0) ? 0.5f : -0.5f;
            else if (cases[c].kind == 4)
                in.samples[i] = (float)(0.4 * sin(2.0 * 3.14159265358979 *
                                                   440.0 * (double)i / 44100.0));
        }

        memset(&out, 0, sizeof(out));
        Err e = trip(&in, 6, &out);
        if (e != ERR_OK) {
            snprintf(det, sizeof(det), "%s", g_err_str(e));
            ok = 0;
        } else if (out.count != in.count || out.channels != in.channels ||
                   out.sample_rate != in.sample_rate) {
            snprintf(det, sizeof(det), "shape changed");
            ok = 0;
        } else if (cases[c].min_snr >= 999.0) {
            ok = same(&in, &out);
            snprintf(det, sizeof(det), "%zu frames", cases[c].frames);
        } else {
            snr = (double)nofft_decode_snr_db(in.samples, out.samples, in.count);
            ok = (snr >= cases[c].min_snr);
            snprintf(det, sizeof(det), "snr=%.2f dB, want >= %.0f",
                     snr, cases[c].min_snr);
        }
        check(ok, verbose, cases[c].name, det);

        pcm_free(&in);
        pcm_free(&out);
    }
}

/* Every accepted bit width has to survive a full file round trip, and the rate
   has to move monotonically with the width. */
static void test_widths(int verbose)
{
    const size_t frames = 8192;
    const uint16_t ch = 2;
    double rates[ACPCM_BITS_MAX + 1];
    pcm_buf in;
    char det[192];

    for (uint16_t b = 0; b <= ACPCM_BITS_MAX; b++)
        rates[b] = -1.0;

    memset(&in, 0, sizeof(in));
    in.count = frames * ch;
    in.channels = ch;
    in.sample_rate = 48000;
    in.samples = (float*)malloc(in.count * sizeof(float));
    if (in.samples == NULL)
        return;

    /* Broadband noise is what keeps a1 off the stability clamp; a signal of
       pure tones has an optimal one step gain above 1 and saturates instead
       of gaining 6 dB per bit, which would make this ladder meaningless. */
    rng_seed(0x1234u);
    for (size_t i = 0; i < frames; i++) {
        double t = (double)i / 48000.0;
        in.samples[i * ch + 0] = (float)(0.35 * sin(t * 2.0 * 3.14159265358979 * 220.0) +
                                        0.25 * sin(t * 2.0 * 3.14159265358979 * 1500.0) +
                                        0.25 * (double)rng_next());
        in.samples[i * ch + 1] = (float)(0.35 * sin(t * 2.0 * 3.14159265358979 * 180.0) +
                                        0.25 * sin(t * 2.0 * 3.14159265358979 * 1900.0) +
                                        0.25 * (double)rng_next());
    }

    for (uint16_t bits = ACPCM_BITS_MIN; bits <= 16; bits++) {
        pcm_buf out;
        char path[512];
        acpcm_info info;
        int ok;

        memset(&out, 0, sizeof(out));
        Err e = trip(&in, bits, &out);
        if (e != ERR_OK) {
            snprintf(det, sizeof(det), "bits=%u %s", bits, g_err_str(e));
            check(0, verbose, "round trip across bit widths", det);
            pcm_free(&out);
            continue;
        }

        ok = (out.count == in.count) && (out.channels == ch) &&
             (out.sample_rate == in.sample_rate);
        snprintf(det, sizeof(det), "bits=%-2u snr=%.2f dB",
                 bits, (double)nofft_decode_snr_db(in.samples, out.samples, in.count));
        check(ok, verbose, "round trip across bit widths", det);
        pcm_free(&out);

        temp_path(path, sizeof(path), "rate");
        if (acpcm_encode(path, &in, bits) == ERR_OK &&
            acpcm_file_info(path, &info) == ERR_OK && info.frames != 0) {
            rates[bits] = 8.0 * (double)info.payload /
                          ((double)info.frames * (double)info.channels);
            remove(path);
        }
    }

    /* The rate climbs only while the quantiser is still the limiting factor.
       Once the step is finer than what the 16 bit header fields can express,
       most residuals round to zero, the rate stops paying for the extra width
       and eventually falls again.  Assert the rising part. */
    {
        int rising = 1;
        double lo = rates[ACPCM_BITS_MIN];
        double mid = 0.0;

        for (uint16_t b = ACPCM_BITS_MIN; b < 8; b++) {
            if (rates[b] < 0.0 || rates[b + 1] <= rates[b])
                rising = 0;
            if (b + 1 == 6)
                mid = rates[b + 1];
        }
        snprintf(det, sizeof(det), "bits2=%.3f bits6=%.3f bits8=%.3f b/sample",
                 lo, mid, rates[8]);
        check(rising, verbose, "rate rises over the useful widths", det);
        snprintf(det, sizeof(det), "bits=2 costs %.3f b/sample", lo);
        check(lo > 0.0 && lo < 2.0, verbose, "coarse width stays cheap", det);
    }

    pcm_free(&in);
}

/* The step has to survive being stored in a 16 bit header field without leaving
   the largest residual outside the symbol range.  When it did not, one clipped
   sample left an error that decayed only as fast as 1/(1-a1) and quality became
   erratic: bits=14 scored worse than bits=12 on real material.  A clamp free
   stream is what monotonic quality in the width ladder rests on. */
static void test_no_clipping(int verbose)
{
    const size_t frames = 20000;
    pcm_buf in, out;
    char det[192];
    double prev_snr = 0.0;
    int rising = 1;
    int any_over = 0;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.count = frames * 2;
    in.channels = 2;
    in.sample_rate = 44100;
    in.samples = (float*)malloc(in.count * sizeof(float));
    if (in.samples == NULL)
        return;

    rng_seed(0x2468u);
    for (size_t i = 0; i < frames; i++) {
        double t = (double)i / 44100.0;
        in.samples[i * 2 + 0] = (float)(0.35 * sin(t * 2.0 * 3.14159265358979 * 220.0) +
                                        0.25 * sin(t * 2.0 * 3.14159265358979 * 1500.0) +
                                        0.25 * (double)rng_next());
        in.samples[i * 2 + 1] = (float)(0.35 * sin(t * 2.0 * 3.14159265358979 * 180.0) +
                                        0.25 * sin(t * 2.0 * 3.14159265358979 * 1900.0) +
                                        0.25 * (double)rng_next());
    }

    /* quality has to climb with the width; a clipped peak makes it dip */
    for (uint16_t bits = 4; bits <= 14; bits++) {
        Err e = trip(&in, bits, &out);
        if (e != ERR_OK) {
            snprintf(det, sizeof(det), "bits=%u %s", bits, g_err_str(e));
            check(0, verbose, "quality climbs with bit width", det);
            pcm_free(&out);
            memset(&out, 0, sizeof(out));
            continue;
        }
        double snr = (double)nofft_decode_snr_db(in.samples, out.samples, in.count);
        if (bits > 4 && snr <= prev_snr)
            rising = 0;
        prev_snr = snr;
        pcm_free(&out);
        memset(&out, 0, sizeof(out));
    }
    check(rising, verbose, "quality climbs with bit width", "");

    /* Overshoot is expected, runaway is not: with a1 held under 1 a clip free
       loop cannot diverge, so a peak far past the input means it did. */
    for (uint16_t bits = 4; bits <= 16; bits += 2) {
        Err e = trip(&in, bits, &out);
        if (e != ERR_OK)
            continue;
        /* compare peaks: a per sample ratio is meaningless where the input
           sample is near zero, and the quantiser legitimately nudges those */
        double pin = 0.0, pout = 0.0;
        for (size_t i = 0; i < in.count; i++) {
            double av = in.samples[i] < 0 ? -(double)in.samples[i]
                                          : (double)in.samples[i];
            double bv = out.samples[i] < 0 ? -(double)out.samples[i]
                                           : (double)out.samples[i];
            if (av > pin)
                pin = av;
            if (bv > pout)
                pout = bv;
        }
        if (pin > 0.0 && pout > 2.0 * pin)
            any_over = 1;
        pcm_free(&out);
        memset(&out, 0, sizeof(out));
    }
    check(!any_over, verbose, "no runaway across widths", "");

    pcm_free(&in);
}

/* Noise is worst case for a DPCM, so it is the cheapest way to prove the
   quantiser actually bounds the signal instead of letting it run away. */
static void test_noise_bound(int verbose)
{
    const size_t frames = 16384;
    pcm_buf in, out;
    char det[192];
    double peak = 0.0;
    int ok;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.count = frames;
    in.channels = 1;
    in.sample_rate = 44100;
    in.samples = (float*)malloc(in.count * sizeof(float));
    if (in.samples == NULL)
        return;

    rng_seed(0xbeefu);
    for (size_t i = 0; i < frames; i++)
        in.samples[i] = rng_next();

    for (uint16_t bits = 2; bits <= 8; bits++) {
        Err e = trip(&in, bits, &out);
        if (e != ERR_OK) {
            snprintf(det, sizeof(det), "bits=%u %s", bits, g_err_str(e));
            check(0, verbose, "noise stays bounded", det);
            pcm_free(&in);
            pcm_free(&out);
            return;
        }
        for (size_t i = 0; i < out.count; i++) {
            double a = out.samples[i] < 0 ? -(double)out.samples[i]
                                          : (double)out.samples[i];
            if (a > peak)
                peak = a;
        }
        if (!(peak == peak))
            break;
        pcm_free(&out);
        memset(&out, 0, sizeof(out));
    }

    /* the input peak is 0.5; a stable first order predictor at a1 < 1 cannot
       run away, so anything near this bound means the loop diverged */
    ok = (peak < 8.0);
    snprintf(det, sizeof(det), "peak=%.3f (input 0.5)", peak);
    check(ok, verbose, "noise stays bounded", det);

    pcm_free(&in);
    pcm_free(&out);
}

static void expect_err(Err got, Err want, int verbose, const char* what)
{
    char det[192];
    snprintf(det, sizeof(det), "got %s, want %s", g_err_str(got), g_err_str(want));
    check(got == want, verbose, what, det);
}

static void test_rejects(int verbose)
{
    pcm_buf in, out;
    char path[512];
    uint8_t buf[4096];
    FILE* f;

    /* input that the 16 bit header fields cannot represent */
    memset(&in, 0, sizeof(in));
    in.count = 512;
    in.channels = 1;
    in.sample_rate = 44100;
    in.samples = (float*)malloc(in.count * sizeof(float));
    if (in.samples == NULL)
        return;
    for (size_t i = 0; i < in.count; i++)
        in.samples[i] = 100.0f;

    temp_path(path, sizeof(path), "hot");
    expect_err(acpcm_encode(path, &in, 6), ERR_BAD_INPUT, verbose,
               "rejects out of range samples");
    in.samples[5] = 0.0f / 0.0f; /* NaN */
    expect_err(acpcm_encode(path, &in, 6), ERR_BAD_INPUT, verbose,
               "rejects NaN samples");
    for (size_t i = 0; i < in.count; i++)
        in.samples[i] = 0.25f;

    expect_err(acpcm_encode(path, &in, 0), ERR_RANGE, verbose,
               "rejects bits below the minimum");
    expect_err(acpcm_encode(path, &in, 255), ERR_RANGE, verbose,
               "rejects bits above the maximum");
    expect_err(acpcm_encode(path, &in, 6), ERR_OK, verbose,
               "accepts a valid encode");

    /* a truncated payload must be an error, not a short buffer */
    memset(&out, 0, sizeof(out));
    {
        FILE* src = fopen(path, "rb");
        long sz = -1;
        size_t got;
        if (src != NULL) {
            fseek(src, 0, SEEK_END);
            sz = ftell(src);
            fseek(src, 0, SEEK_SET);
            got = (sz > 0) ? fread(buf, 1, (size_t)sz, src) : 0;
            fclose(src);
            /* keep the fixed header and the length table, drop most of the
               payload, so the failure has to come from a short read */
            if (sz > 0 && got > ACPCM_FIXED_HEADER + 4)
                got = ACPCM_FIXED_HEADER + 4 + (got - ACPCM_FIXED_HEADER - 4) / 2;
            else
                got = (got > 1) ? got - 1 : got;
            f = fopen(path, "wb");
            if (f != NULL) {
                fwrite(buf, 1, got, f);
                fclose(f);
            }
        }
        expect_err(acpcm_decode(path, &out), ERR_IO, verbose,
                   "rejects a truncated file");
        pcm_free(&out);
    }

    /* wrong magic */
    memset(buf, 0, sizeof(buf));
    memcpy(buf, "NOFF", 4);
    f = fopen(path, "wb");
    if (f != NULL) {
        fwrite(buf, 1, sizeof(buf), f);
        fclose(f);
    }
    memset(&out, 0, sizeof(out));
    expect_err(acpcm_decode(path, &out), ERR_BAD_NOFFT, verbose,
               "rejects a foreign magic");
    pcm_free(&out);

    /* a .nadc must not be readable as a .no_fft and the other way round */
    expect_err(acpcm_encode(path, &in, 6), ERR_OK, verbose,
               "writes a valid file for the cross check");
    {
        nofft_fit f2;
        Err e = nofft_file_fit(path, &f2);
        check(e != ERR_OK, verbose, "a .nadc is not a .no_fft", NULL);
    }

    remove(path);
    pcm_free(&in);
}

/* NFA2: a basket meter watches the residuals and asks for an a1 refit when
   they start costing bits.  What must hold: the never policy is bit identical
   to NFA1 (reading boundary flags alone must not touch the audio), flat
   signals stay exact under every policy, and a nonstationary signal at least
   decodes as well as it did with one global predictor.  The rate table is the
   experiment, reported not asserted: on stationary material drift may lose. */
static void test_drift(int verbose)
{
    const size_t frames = 12000;
    const uint32_t bl = 1024;
    pcm_buf in, o1, o2;
    char det[192];
    double snr1 = 0.0;
    double bps1 = 0.0, bps_never = 0.0, bps_always = 0.0, bps_drift = 0.0;

    memset(&in, 0, sizeof(in));
    memset(&o1, 0, sizeof(o1));
    memset(&o2, 0, sizeof(o2));
    in.count = frames;
    in.channels = 1;
    in.sample_rate = 44100;
    in.samples = (float*)malloc(in.count * sizeof(float));
    if (in.samples == NULL)
        return;

    /* first half: a smooth low tone, whose best one step gain sits on the
       stability clamp; second half: white noise, whose best gain is 0.  One
       global fit has to compromise, so a refit at the midpoint is worth
       real bits. */
    rng_seed(0xD1FFu);
    for (size_t i = 0; i < frames; i++) {
        if (i < frames / 2)
            in.samples[i] = (float)(0.4 * sin(2.0 * 3.14159265358979 *
                                              110.0 * (double)i / 44100.0));
        else
            in.samples[i] = 0.35f * rng_next();
    }

    /* invariant a: v2 never == v1 */
    Err e = trip_x(&in, 6, 0, 0, ACP2_NEVER, &o1, &bps1);
    if (e == ERR_OK)
        e = trip_x(&in, 6, 1, bl, ACP2_NEVER, &o2, &bps_never);
    if (e != ERR_OK || o1.count != frames || o2.count != frames) {
        snprintf(det, sizeof(det), "%s", g_err_str(e));
        check(0, verbose, "nfa2 round trips", det);
        pcm_free(&in);
        pcm_free(&o1);
        pcm_free(&o2);
        return;
    }
    check(same(&o1, &o2), verbose, "v2 never decodes bit identical to v1",
          "boundary flags must not touch the audio");
    snr1 = (double)nofft_decode_snr_db(in.samples, o1.samples, in.count);

    /* invariant b: flat signals stay exact under the refitting policies */
    {
        static const struct {
            const char* name;
            float val;
            acpcm_p2_policy pol;
        } flat[] = {
            { "silence exact under drift", 0.0f, ACP2_DRIFT },
            { "dc exact under always",     0.75f, ACP2_ALWAYS },
        };
        for (size_t k = 0; k < sizeof(flat) / sizeof(flat[0]); k++) {
            pcm_buf fi, fo;
            Err fe;
            memset(&fi, 0, sizeof(fi));
            memset(&fo, 0, sizeof(fo));
            fi.count = 4096;
            fi.channels = 1;
            fi.sample_rate = 44100;
            fi.samples = (float*)malloc(fi.count * sizeof(float));
            if (fi.samples == NULL)
                break;
            for (size_t i = 0; i < fi.count; i++)
                fi.samples[i] = flat[k].val;
            fe = trip_x(&fi, 6, 1, bl, flat[k].pol, &fo, NULL);
            check(fe == ERR_OK && fo.count == fi.count && same(&fi, &fo),
                  verbose, flat[k].name, NULL);
            pcm_free(&fi);
            pcm_free(&fo);
        }
    }

    /* invariant c: the nonstationary signal under the refitting policies */
    {
        static const struct {
            const char* name;
            acpcm_p2_policy pol;
            double* bps;
        } pols[] = {
            { "v2 always holds quality", ACP2_ALWAYS, &bps_always },
            { "v2 drift holds quality",  ACP2_DRIFT,  &bps_drift },
        };
        for (size_t k = 0; k < sizeof(pols) / sizeof(pols[0]); k++) {
            pcm_buf fo;
            double snr;
            Err fe;
            memset(&fo, 0, sizeof(fo));
            fe = trip_x(&in, 6, 1, bl, pols[k].pol, &fo, pols[k].bps);
            snr = (fe == ERR_OK && fo.count == in.count)
                      ? (double)nofft_decode_snr_db(in.samples, fo.samples, in.count)
                      : -99.0;
            snprintf(det, sizeof(det), "snr=%.2f dB (v1 %.2f), %.4f b/sample",
                     snr, snr1, *pols[k].bps);
            check(fe == ERR_OK && fo.count == in.count && snr >= snr1 - 2.0,
                  verbose, pols[k].name, det);
            pcm_free(&fo);
        }
    }

    /* experiment d: what the refit machinery costs and buys */
    snprintf(det, sizeof(det), "v1 %.4f  never %.4f  always %.4f  drift %.4f",
             bps1, bps_never, bps_always, bps_drift);
    check(1, verbose, "rate table (b/sample, bits=6)", det);

    /* invariant e: malformed NFA2 is rejected, not misparsed */
    {
        char path[512];
        pcm_buf fo;

        temp_path(path, sizeof(path), "drift");
        expect_err(acpcm_encode2(path, &in, 6, 0, ACP2_DRIFT), ERR_RANGE,
                   verbose, "rejects block_len 0");
        expect_err(acpcm_encode2(path, &in, 6, bl, (acpcm_p2_policy)9),
                   ERR_RANGE, verbose, "rejects an unknown policy");

        e = acpcm_encode2(path, &in, 6, bl, ACP2_DRIFT);
        check(e == ERR_OK, verbose, "writes a valid nfa2 file",
              e == ERR_OK ? NULL : g_err_str(e));
        if (e == ERR_OK) {
            FILE* f2 = fopen(path, "r+b");
            if (f2 != NULL) {
                uint8_t z[4] = { 0, 0, 0, 0 };
                fseek(f2, ACPCM_FIXED_HEADER, SEEK_SET);
                fwrite(z, 1, sizeof(z), f2);
                fclose(f2);
            }
            memset(&fo, 0, sizeof(fo));
            expect_err(acpcm_decode(path, &fo), ERR_BAD_NOFFT, verbose,
                       "rejects block_len 0 in the header");
            pcm_free(&fo);
        }
        remove(path);
    }

    pcm_free(&in);
    pcm_free(&o1);
    pcm_free(&o2);
}

/* NFA3: basket coding.  What holds: flat signals stay bit exact, the
   nonstationary signal keeps its SNR within 2 dB of NFA1, spike transients
   (which can saturate the residual and take the raw escape path) still decode
   cleanly, and the file really is an NFA3 container.  The rate table is
   reported: whether a histogram-coded basket beats the adaptive unary run is
   the experiment. */
static void test_baskets(int verbose)
{
    const size_t frames = 12000;
    pcm_buf in, o1, o3;
    char det[192];
    double bps1 = 0.0, bps3 = 0.0, snr1 = 0.0, snr3 = 0.0;

    memset(&in, 0, sizeof(in));
    memset(&o1, 0, sizeof(o1));
    memset(&o3, 0, sizeof(o3));
    in.count = frames;
    in.channels = 1;
    in.sample_rate = 44100;
    in.samples = (float*)malloc(in.count * sizeof(float));
    if (in.samples == NULL)
        return;

    rng_seed(0xB4A5u);
    for (size_t i = 0; i < frames; i++) {
        if (i < frames / 2)
            in.samples[i] = (float)(0.4 * sin(2.0 * 3.14159265358979 *
                                              110.0 * (double)i / 44100.0));
        else
            in.samples[i] = 0.35f * rng_next();
    }

    Err e = trip_x(&in, 6, 0, 0, ACP2_NEVER, &o1, &bps1);
    if (e == ERR_OK) {
        char path[512];
        temp_path(path, sizeof(path), "b3");
        e = acpcm_encode3(path, &in, 6);
        if (e == ERR_OK) {
            acpcm_info info;
            memset(&info, 0, sizeof(info));
            if (acpcm_file_info(path, &info) == ERR_OK && info.frames != 0) {
                bps3 = 8.0 * (double)info.payload /
                       ((double)info.frames * (double)info.channels);
                snprintf(det, sizeof(det), "format=%u block=%u (want 2 and 0)",
                         (unsigned)info.format, (unsigned)info.block_len);
                check(info.format == 2 && info.block_len == 0, verbose,
                      "nfa3 file reports format 2, block 0", det);
            }
            e = acpcm_decode(path, &o3);
        }
        remove(path);
    }
    if (e != ERR_OK || o1.count != frames || o3.count != frames) {
        snprintf(det, sizeof(det), "%s", g_err_str(e));
        check(0, verbose, "nfa3 round trips", det);
        pcm_free(&in);
        pcm_free(&o1);
        pcm_free(&o3);
        return;
    }
    snr1 = (double)nofft_decode_snr_db(in.samples, o1.samples, in.count);
    snr3 = (double)nofft_decode_snr_db(in.samples, o3.samples, in.count);
    snprintf(det, sizeof(det), "snr=%.2f dB (v1 %.2f), %.4f b/sample (v1 %.4f)",
             snr3, snr1, bps3, bps1);
    check(snr3 >= snr1 - 2.0, verbose, "baskets hold nonstationary quality", det);

    /* flat signals stay exact */
    {
        static const float vals[] = { 0.0f, 0.75f };
        for (size_t k = 0; k < sizeof(vals) / sizeof(vals[0]); k++) {
            pcm_buf fi, fo;
            Err fe;
            memset(&fi, 0, sizeof(fi));
            memset(&fo, 0, sizeof(fo));
            fi.count = 4096;
            fi.channels = 1;
            fi.sample_rate = 44100;
            fi.samples = (float*)malloc(fi.count * sizeof(float));
            if (fi.samples == NULL)
                break;
            for (size_t i = 0; i < fi.count; i++)
                fi.samples[i] = vals[k];
            char path[512];
            temp_path(path, sizeof(path), "b3f");
            fe = acpcm_encode3(path, &fi, 6);
            if (fe == ERR_OK)
                fe = acpcm_decode(path, &fo);
            remove(path);
            check(fe == ERR_OK && fo.count == fi.count && same(&fi, &fo),
                  verbose, vals[k] == 0.0f ? "silence exact in baskets"
                                           : "dc exact in baskets", NULL);
            pcm_free(&fi);
            pcm_free(&fo);
        }
    }

    /* spike transients: a low floor with sparse loud impulses.  The drops can
       saturate the residual and take the raw escape path; whichever branch
       runs, the decode must lose nothing audible. */
    {
        pcm_buf si, so;
        double ssnr;
        Err fe;
        memset(&si, 0, sizeof(si));
        memset(&so, 0, sizeof(so));
        si.count = frames;
        si.channels = 1;
        si.sample_rate = 44100;
        si.samples = (float*)malloc(si.count * sizeof(float));
        if (si.samples != NULL) {
            for (size_t i = 0; i < si.count; i++)
                si.samples[i] = 0.05f;
            for (size_t k = 0; k < 8; k++)
                si.samples[200 + k * 1400] = (k % 2) ? -1.60f : 1.60f;
            char path[512];
            temp_path(path, sizeof(path), "b3s");
            fe = acpcm_encode3(path, &si, 6);
            if (fe == ERR_OK)
                fe = acpcm_decode(path, &so);
            remove(path);
            ssnr = (fe == ERR_OK && so.count == si.count)
                       ? (double)nofft_decode_snr_db(si.samples, so.samples,
                                                     si.count)
                       : -99.0;
            snprintf(det, sizeof(det), "snr=%.2f dB", ssnr);
            check(fe == ERR_OK && so.count == si.count && ssnr >= 30.0,
                  verbose, "spike transients decode", det);
            pcm_free(&si);
            pcm_free(&so);
        }
    }

    pcm_free(&in);
    pcm_free(&o1);
    pcm_free(&o3);
}

/* NFA4: the basket-only codec.  Everything must round trip: the shifted grid
   and the per-sample adaptive basket histogram are the whole stream.  A finer
   basket count must beat a coarser one on the same signal, a 170-basket grid
   must work even though the integer-percent step caps at 101 rows, and a flat
   channel (peak_q == 0) has every sample in bucket 0 whose midpoint is 0, so
   silence and DC stay exact. */
static void test_nfa4(int verbose)
{
    const size_t frames = 12000;
    pcm_buf in, o4;
    char det[192];
    double bps4 = 0.0, snr4 = 0.0;

    memset(&in, 0, sizeof(in));
    memset(&o4, 0, sizeof(o4));
    in.count = frames * 2;          /* stereo, exercises per-channel tables */
    in.channels = 2;
    in.sample_rate = 44100;
    in.samples = (float*)malloc(in.count * sizeof(float));
    if (in.samples == NULL)
        return;

    rng_seed(0xC41Du);
    for (size_t i = 0; i < frames; i++) {
        in.samples[2 * i + 0] = (float)(0.6 * sin(2.0 * 3.14159265358979 *
                                                  440.0 * (double)i / 44100.0));
        in.samples[2 * i + 1] = 0.3f * rng_next();
    }

    char path[512];
    temp_path(path, sizeof(path), "n4");
    Err e = acpcm_encode4(path, &in, 16, 5);
    if (e == ERR_OK) {
        acpcm_info info;
        memset(&info, 0, sizeof(info));
        if (acpcm_file_info(path, &info) == ERR_OK && info.frames != 0) {
            bps4 = 8.0 * (double)info.payload /
                   ((double)info.frames * (double)info.channels);
            snprintf(det, sizeof(det), "format=%u block=%u (want 3 and 0)",
                     (unsigned)info.format, (unsigned)info.block_len);
            check(info.format == 3 && info.block_len == 0, verbose,
                  "nfa4 file reports format 3, block 0", det);
        }
        e = acpcm_decode(path, &o4);
    }
    if (e != ERR_OK || o4.count != in.count) {
        snprintf(det, sizeof(det), "%s", g_err_str(e));
        check(0, verbose, "nfa4 round trips", det);
        pcm_free(&in);
        pcm_free(&o4);
        return;
    }
    snr4 = (double)nofft_decode_snr_db(in.samples, o4.samples, in.count);
    snprintf(det, sizeof(det), "snr=%.2f dB, %.4f b/sample", snr4, bps4);
    check(snr4 >= 12.0, verbose, "nfa4 codes the basket grid", det);

    /* a finer basket grid must beat a coarser one on the same signal */
    {
        pcm_buf fine, coarse;
        double snr_fine = -99.0, snr_coarse = -99.0;
        char p1[512], p2[512];
        memset(&fine, 0, sizeof(fine));
        memset(&coarse, 0, sizeof(coarse));
        temp_path(p1, sizeof(p1), "n4a");
        temp_path(p2, sizeof(p2), "n4b");
        if (acpcm_encode4(p1, &in, 16, 1) == ERR_OK &&
            acpcm_decode(p1, &fine) == ERR_OK && fine.count == in.count)
            snr_fine = (double)nofft_decode_snr_db(in.samples, fine.samples,
                                                   in.count);
        if (acpcm_encode4(p2, &in, 16, 20) == ERR_OK &&
            acpcm_decode(p2, &coarse) == ERR_OK && coarse.count == in.count)
            snr_coarse = (double)nofft_decode_snr_db(in.samples, coarse.samples,
                                                     in.count);
        remove(p1);
        remove(p2);
        snprintf(det, sizeof(det), "step=1 %.2f dB vs step=20 %.2f dB",
                 snr_fine, snr_coarse);
        check(snr_fine > snr_coarse + 3.0, verbose,
              "nfa4 finer step improves fidelity", det);
        pcm_free(&fine);
        pcm_free(&coarse);
    }

    /* The integer-percent grid caps at 101 rows, so a 170-basket grid needs the
       count-based entry point.  It must round trip and beat 101 baskets on the
       same signal. */
    {
        pcm_buf fine, mid;
        double snr170 = -99.0, snr101 = -99.0;
        char p1[512], p2[512];
        memset(&fine, 0, sizeof(fine));
        memset(&mid, 0, sizeof(mid));
        temp_path(p1, sizeof(p1), "n4c");
        temp_path(p2, sizeof(p2), "n4d");
        if (acpcm_encode4_baskets(p1, &in, 16, 170) == ERR_OK &&
            acpcm_decode(p1, &fine) == ERR_OK && fine.count == in.count)
            snr170 = (double)nofft_decode_snr_db(in.samples, fine.samples,
                                                 in.count);
        if (acpcm_encode4_baskets(p2, &in, 16, 101) == ERR_OK &&
            acpcm_decode(p2, &mid) == ERR_OK && mid.count == in.count)
            snr101 = (double)nofft_decode_snr_db(in.samples, mid.samples,
                                                 in.count);
        remove(p1);
        remove(p2);
        snprintf(det, sizeof(det), "170 baskets %.2f dB vs 101 baskets %.2f dB",
                 snr170, snr101);
        check(snr170 > snr101 + 1.0, verbose,
              "nfa4 170-basket grid beats 101", det);
        pcm_free(&fine);
        pcm_free(&mid);
    }

    /* flat signals stay exact */
    {
        static const float vals[] = { 0.0f, 0.75f };
        for (size_t k = 0; k < sizeof(vals) / sizeof(vals[0]); k++) {
            pcm_buf fi, fo;
            Err fe;
            memset(&fi, 0, sizeof(fi));
            memset(&fo, 0, sizeof(fo));
            fi.count = 4096;
            fi.channels = 1;
            fi.sample_rate = 44100;
            fi.samples = (float*)malloc(fi.count * sizeof(float));
            if (fi.samples == NULL)
                break;
            for (size_t i = 0; i < fi.count; i++)
                fi.samples[i] = vals[k];
            char fp[512];
            temp_path(fp, sizeof(fp), "n4f");
            fe = acpcm_encode4(fp, &fi, 16, 5);
            if (fe == ERR_OK)
                fe = acpcm_decode(fp, &fo);
            remove(fp);
            check(fe == ERR_OK && fo.count == fi.count && same(&fi, &fo),
                  verbose, vals[k] == 0.0f ? "silence exact in nfa4"
                                           : "dc exact in nfa4", NULL);
            pcm_free(&fi);
            pcm_free(&fo);
        }
    }

    /* a signal far from the centre with sparse dips: the base shift keeps the
       whole channel non-negative and the basket grid codes it in range */
    {
        pcm_buf si, so;
        double ssnr;
        Err fe;
        memset(&si, 0, sizeof(si));
        memset(&so, 0, sizeof(so));
        si.count = frames;
        si.channels = 1;
        si.sample_rate = 44100;
        si.samples = (float*)malloc(si.count * sizeof(float));
        if (si.samples != NULL) {
            for (size_t i = 0; i < si.count; i++)
                si.samples[i] = 0.9f;
            for (size_t k = 0; k < 24; k++)
                si.samples[100 + k * 500] = -0.4f;
            char sp[512];
            temp_path(sp, sizeof(sp), "n4s");
            fe = acpcm_encode4(sp, &si, 16, 1);
            if (fe == ERR_OK)
                fe = acpcm_decode(sp, &so);
            remove(sp);
            ssnr = (fe == ERR_OK && so.count == si.count)
                       ? (double)nofft_decode_snr_db(si.samples, so.samples,
                                                     si.count)
                       : -99.0;
            snprintf(det, sizeof(det), "snr=%.2f dB", ssnr);
            check(fe == ERR_OK && so.count == si.count && ssnr >= 40.0,
                  verbose, "nfa4 codes an off-centre range", det);
            pcm_free(&si);
            pcm_free(&so);
        }
    }

    /* the flags are validated before any file is written */
    expect_err(acpcm_encode4(path, &in, 16, 0), ERR_RANGE, verbose,
               "nfa4 rejects step below the minimum");
    expect_err(acpcm_encode4(path, &in, 16, 70000), ERR_RANGE, verbose,
               "nfa4 rejects step above the maximum");
    expect_err(acpcm_encode4(path, &in, 0, 5), ERR_RANGE, verbose,
               "nfa4 rejects bits below the minimum");
    expect_err(acpcm_encode4_baskets(path, &in, 16, 0), ERR_RANGE, verbose,
               "nfa4 rejects a zero basket count");
    expect_err(acpcm_encode4_baskets(path, &in, 16, ACPCM4_MAX_BUCKETS + 1),
               ERR_RANGE, verbose, "nfa4 rejects a basket count over the max");
    expect_err(acpcm_encode4_baskets(path, &in, 0, 170), ERR_RANGE, verbose,
               "nfa4 baskets rejects bits below the minimum");

    pcm_free(&in);
    pcm_free(&o4);
}

/* NFA5: the multiplicative (log-domain) predictor.  Unlike NFA1-4 it is lossy
   by construction, since a geometric grid cannot hit every integer, so the
   checks are quality bounds rather than equality: a finer err must buy SNR and
   cost rate, a flat signal is exact (K = 0 on every step), a one frame chunk
   is just the seed, and the flags are validated before anything is written. */
static void test_nfa5(int verbose)
{
    const size_t frames = 12000;
    pcm_buf in, o5;
    char det[192];
    double bps5 = 0.0, snr5 = 0.0;

    memset(&in, 0, sizeof(in));
    memset(&o5, 0, sizeof(o5));
    in.count = frames * 2;          /* stereo, exercises per-channel grids */
    in.channels = 2;
    in.sample_rate = 44100;
    in.samples = (float*)malloc(in.count * sizeof(float));
    if (in.samples == NULL)
        return;

    rng_seed(0x5A5Au);
    for (size_t i = 0; i < frames; i++) {
        in.samples[2 * i + 0] = (float)(0.6 * sin(2.0 * 3.14159265358979 *
                                                  440.0 * (double)i / 44100.0));
        in.samples[2 * i + 1] = 0.3f * rng_next();
    }

    char path[512];
    temp_path(path, sizeof(path), "n5");
    Err e = acpcm_encode5(path, &in, 16, 2.0, 0.01);
    if (e == ERR_OK) {
        acpcm_info info;
        memset(&info, 0, sizeof(info));
        if (acpcm_file_info(path, &info) == ERR_OK && info.frames != 0) {
            bps5 = 8.0 * (double)info.payload /
                   ((double)info.frames * (double)info.channels);
            snprintf(det, sizeof(det), "format=%u block=%u (want 4 and 0)",
                     (unsigned)info.format, (unsigned)info.block_len);
            check(info.format == 4 && info.block_len == 0, verbose,
                  "nfa5 file reports format 4, block 0", det);
        }
        e = acpcm_decode(path, &o5);
    }
    if (e != ERR_OK || o5.count != in.count) {
        snprintf(det, sizeof(det), "%s", g_err_str(e));
        check(0, verbose, "nfa5 round trips", det);
        pcm_free(&in);
        pcm_free(&o5);
        return;
    }
    snr5 = (double)nofft_decode_snr_db(in.samples, o5.samples, in.count);
    snprintf(det, sizeof(det), "snr=%.2f dB, %.4f b/sample", snr5, bps5);
    check(snr5 >= 25.0, verbose, "nfa5 codes the log grid", det);

    /* a finer log step must buy fidelity on the same signal */
    {
        pcm_buf fine, coarse;
        double snr_fine = -99.0, snr_coarse = -99.0;
        char p1[512], p2[512];
        memset(&fine, 0, sizeof(fine));
        memset(&coarse, 0, sizeof(coarse));
        temp_path(p1, sizeof(p1), "n5a");
        temp_path(p2, sizeof(p2), "n5b");
        if (acpcm_encode5(p1, &in, 16, 2.0, 0.001) == ERR_OK &&
            acpcm_decode(p1, &fine) == ERR_OK && fine.count == in.count)
            snr_fine = (double)nofft_decode_snr_db(in.samples, fine.samples,
                                                   in.count);
        if (acpcm_encode5(p2, &in, 16, 2.0, 0.05) == ERR_OK &&
            acpcm_decode(p2, &coarse) == ERR_OK && coarse.count == in.count)
            snr_coarse = (double)nofft_decode_snr_db(in.samples, coarse.samples,
                                                     in.count);
        remove(p1);
        remove(p2);
        snprintf(det, sizeof(det), "err=0.001 %.2f dB vs err=0.05 %.2f dB",
                 snr_fine, snr_coarse);
        check(snr_fine > snr_coarse + 10.0, verbose,
              "nfa5 finer err improves fidelity", det);
        pcm_free(&fine);
        pcm_free(&coarse);
    }

    /* flat signals are exact: u never changes, so every K is 0 */
    {
        static const float vals[] = { 0.0f, 0.75f };
        for (size_t k = 0; k < sizeof(vals) / sizeof(vals[0]); k++) {
            pcm_buf fi, fo;
            Err fe;
            memset(&fi, 0, sizeof(fi));
            memset(&fo, 0, sizeof(fo));
            fi.count = 4096;
            fi.channels = 1;
            fi.sample_rate = 44100;
            fi.samples = (float*)malloc(fi.count * sizeof(float));
            if (fi.samples == NULL)
                break;
            for (size_t i = 0; i < fi.count; i++)
                fi.samples[i] = vals[k];
            char fp[512];
            temp_path(fp, sizeof(fp), "n5f");
            fe = acpcm_encode5(fp, &fi, 16, 2.0, 0.01);
            if (fe == ERR_OK)
                fe = acpcm_decode(fp, &fo);
            remove(fp);
            check(fe == ERR_OK && fo.count == fi.count && same(&fi, &fo),
                  verbose, vals[k] == 0.0f ? "silence exact in nfa5"
                                           : "dc exact in nfa5", NULL);
            pcm_free(&fi);
            pcm_free(&fo);
        }
    }

    /* a one frame chunk is the seed and nothing else */
    {
        pcm_buf one, back;
        Err fe;
        memset(&one, 0, sizeof(one));
        memset(&back, 0, sizeof(back));
        one.count = 1;
        one.channels = 1;
        one.sample_rate = 44100;
        one.samples = (float*)malloc(sizeof(float));
        if (one.samples != NULL) {
            one.samples[0] = 0.5f;
            temp_path(path, sizeof(path), "n5o");
            fe = acpcm_encode5(path, &one, 16, 2.0, 0.01);
            if (fe == ERR_OK)
                fe = acpcm_decode(path, &back);
            remove(path);
            check(fe == ERR_OK && back.count == 1 && same(&one, &back), verbose,
                  "nfa5 one frame is just the seed", NULL);
            pcm_free(&back);
        }
        pcm_free(&one);
    }

    /* the auto-dial picks one global err from a target rate and round trips */
    {
        pcm_buf back;
        double bps_a = 0.0, snr_a = -99.0;
        memset(&back, 0, sizeof(back));
        temp_path(path, sizeof(path), "n5au");
        Err ae = acpcm_encode5_auto(path, &in, 16, 2.0, 2.0);
        if (ae == ERR_OK) {
            acpcm_info info;
            memset(&info, 0, sizeof(info));
            if (acpcm_file_info(path, &info) == ERR_OK && info.frames != 0)
                bps_a = 8.0 * (double)info.payload /
                        ((double)info.frames * (double)info.channels);
            ae = acpcm_decode(path, &back);
        }
        if (ae == ERR_OK && back.count == in.count)
            snr_a = (double)nofft_decode_snr_db(in.samples, back.samples,
                                                in.count);
        remove(path);
        snprintf(det, sizeof(det), "snr=%.2f dB, %.4f b/sample", snr_a, bps_a);
        check(ae == ERR_OK && back.count == in.count && bps_a > 0.5 &&
              bps_a < 3.5 && snr_a > 5.0, verbose,
              "nfa5 auto-dial round trips near its target", det);
        pcm_free(&back);
    }

    /* the point of the auto-dial: a quiet signal must not collapse the way a
       coarse err dialed by ear on loud material does */
    {
        pcm_buf quiet, qa, qc;
        double snr_auto = -99.0, snr_coarse = -99.0, bps_auto = 0.0;
        memset(&quiet, 0, sizeof(quiet));
        memset(&qa, 0, sizeof(qa));
        memset(&qc, 0, sizeof(qc));
        quiet.count = 8820;
        quiet.channels = 1;
        quiet.sample_rate = 44100;
        quiet.samples = (float*)malloc(quiet.count * sizeof(float));
        if (quiet.samples != NULL) {
            for (size_t i = 0; i < quiet.count; i++)
                quiet.samples[i] = (float)(0.1 * sin(2.0 * 3.14159265358979 *
                                                     440.0 * (double)i / 44100.0));
            char pa[512], pc[512];
            temp_path(pa, sizeof(pa), "n5qauto");
            temp_path(pc, sizeof(pc), "n5qfix");
            if (acpcm_encode5_auto(pa, &quiet, 16, 2.0, 2.0) == ERR_OK &&
                acpcm_decode(pa, &qa) == ERR_OK && qa.count == quiet.count) {
                snr_auto = (double)nofft_decode_snr_db(quiet.samples,
                                                       qa.samples, quiet.count);
                acpcm_info info;
                memset(&info, 0, sizeof(info));
                if (acpcm_file_info(pa, &info) == ERR_OK && info.frames != 0)
                    bps_auto = 8.0 * (double)info.payload /
                               ((double)info.frames * (double)info.channels);
            }
            if (acpcm_encode5(pc, &quiet, 16, 2.0, 0.18) == ERR_OK &&
                acpcm_decode(pc, &qc) == ERR_OK && qc.count == quiet.count)
                snr_coarse = (double)nofft_decode_snr_db(quiet.samples,
                                                         qc.samples,
                                                         quiet.count);
            remove(pa);
            remove(pc);
            snprintf(det, sizeof(det),
                     "auto %.2f dB (%.4f b/s) vs fixed err=0.18 %.2f dB",
                     snr_auto, bps_auto, snr_coarse);
            check(snr_auto > snr_coarse + 10.0, verbose,
                  "nfa5 auto-dial rescues a quiet signal", det);
            pcm_free(&qa);
            pcm_free(&qc);
        }
        pcm_free(&quiet);
    }

    /* the auto-dial parameters are validated before any file is written */
    expect_err(acpcm_encode5_auto(path, &in, 16, 1.0, 2.0), ERR_RANGE, verbose,
               "nfa5 auto-dial rejects e <= 1");
    expect_err(acpcm_encode5_auto(path, &in, 16, 2.0, 0.0), ERR_RANGE, verbose,
               "nfa5 auto-dial rejects rate 0");
    expect_err(acpcm_encode5_auto(path, &in, 16, 2.0, 65.0), ERR_RANGE, verbose,
               "nfa5 auto-dial rejects rate above the maximum");

    /* the parameters are validated before any file is written */
    expect_err(acpcm_encode5(path, &in, 16, 1.0, 0.01), ERR_RANGE, verbose,
               "nfa5 rejects e <= 1");
    expect_err(acpcm_encode5(path, &in, 16, 0.5, 0.01), ERR_RANGE, verbose,
               "nfa5 rejects e < 1");
    expect_err(acpcm_encode5(path, &in, 16, 2.0, 0.0), ERR_RANGE, verbose,
               "nfa5 rejects err 0");
    expect_err(acpcm_encode5(path, &in, 16, 2.0, ACPCM5_ERR_MIN / 2.0),
               ERR_RANGE, verbose, "nfa5 rejects err below the minimum");
    expect_err(acpcm_encode5(path, &in, 16, 2.0, ACPCM5_ERR_MAX * 2.0),
               ERR_RANGE, verbose, "nfa5 rejects err above the maximum");
    expect_err(acpcm_encode5(path, &in, 0, 2.0, 0.01), ERR_RANGE, verbose,
               "nfa5 rejects bits below the minimum");
    in.samples[7] = 0.0f / 0.0f;   /* NaN */
    expect_err(acpcm_encode5(path, &in, 16, 2.0, 0.01), ERR_BAD_INPUT, verbose,
               "nfa5 rejects NaN samples");
    in.samples[7] = 0.0f;

    pcm_free(&in);
    pcm_free(&o5);
}

static void test_nfa6(int verbose)
{
    const size_t frames = 12000;
    pcm_buf in, o6;
    char det[192];
    double bps6 = 0.0, snr6 = 0.0;

    memset(&in, 0, sizeof(in));
    memset(&o6, 0, sizeof(o6));
    in.count = frames * 2;          /* stereo, exercises per-channel grids */
    in.channels = 2;
    in.sample_rate = 44100;
    in.samples = (float*)malloc(in.count * sizeof(float));
    if (in.samples == NULL)
        return;

    rng_seed(0x6A6Au);
    for (size_t i = 0; i < frames; i++) {
        in.samples[2 * i + 0] = (float)(0.6 * sin(2.0 * 3.14159265358979 *
                                                  440.0 * (double)i / 44100.0));
        in.samples[2 * i + 1] = 0.3f * rng_next();
    }

    char path[512];
    temp_path(path, sizeof(path), "n6");
    Err e = acpcm_encode6(path, &in, 16, 256, 2.0, 2.6);
    if (e == ERR_OK) {
        acpcm_info info;
        memset(&info, 0, sizeof(info));
        if (acpcm_file_info(path, &info) == ERR_OK && info.frames != 0) {
            bps6 = 8.0 * (double)info.payload /
                   ((double)info.frames * (double)info.channels);
            snprintf(det, sizeof(det), "format=%u block=%u (want 5 and 256)",
                     (unsigned)info.format, (unsigned)info.block_len);
            check(info.format == 5 && info.block_len == 256, verbose,
                  "nfa6 file reports format 5 and its block", det);
        }
        e = acpcm_decode(path, &o6);
    }
    if (e != ERR_OK || o6.count != in.count) {
        snprintf(det, sizeof(det), "%s", g_err_str(e));
        check(0, verbose, "nfa6 round trips", det);
        pcm_free(&in);
        pcm_free(&o6);
        return;
    }
    snr6 = (double)nofft_decode_snr_db(in.samples, o6.samples, in.count);
    snprintf(det, sizeof(det), "snr=%.2f dB, %.4f b/sample", snr6, bps6);
    check(snr6 >= 20.0, verbose, "nfa6 codes the log grid", det);

    /* the per-block rate control must spend more on a higher target */
    {
        double bps_lo = 0.0, bps_hi = 0.0;
        char p1[512], p2[512];
        acpcm_info i1, i2;
        temp_path(p1, sizeof(p1), "n6lo");
        temp_path(p2, sizeof(p2), "n6hi");
        memset(&i1, 0, sizeof(i1));
        memset(&i2, 0, sizeof(i2));
        if (acpcm_encode6(p1, &in, 16, 256, 2.0, 1.5) == ERR_OK &&
            acpcm_file_info(p1, &i1) == ERR_OK && i1.frames != 0)
            bps_lo = 8.0 * (double)i1.payload /
                     ((double)i1.frames * (double)i1.channels);
        if (acpcm_encode6(p2, &in, 16, 256, 2.0, 3.5) == ERR_OK &&
            acpcm_file_info(p2, &i2) == ERR_OK && i2.frames != 0)
            bps_hi = 8.0 * (double)i2.payload /
                     ((double)i2.frames * (double)i2.channels);
        remove(p1);
        remove(p2);
        snprintf(det, sizeof(det), "rate=1.5 %.4f vs rate=3.5 %.4f b/sample",
                 bps_lo, bps_hi);
        check(bps_hi > bps_lo + 0.2, verbose,
              "nfa6 rate control spends more on a higher target", det);
    }

    /* flat signals are exact: u never changes, so every K is 0 */
    {
        static const float vals[] = { 0.0f, 0.75f };
        for (size_t k = 0; k < sizeof(vals) / sizeof(vals[0]); k++) {
            pcm_buf fi, fo;
            Err fe;
            memset(&fi, 0, sizeof(fi));
            memset(&fo, 0, sizeof(fo));
            fi.count = 4096;
            fi.channels = 1;
            fi.sample_rate = 44100;
            fi.samples = (float*)malloc(fi.count * sizeof(float));
            if (fi.samples == NULL)
                break;
            for (size_t i = 0; i < fi.count; i++)
                fi.samples[i] = vals[k];
            char fp[512];
            temp_path(fp, sizeof(fp), "n6f");
            fe = acpcm_encode6(fp, &fi, 16, 256, 2.0, 2.6);
            if (fe == ERR_OK)
                fe = acpcm_decode(fp, &fo);
            remove(fp);
            check(fe == ERR_OK && fo.count == fi.count && same(&fi, &fo),
                  verbose, vals[k] == 0.0f ? "silence exact in nfa6"
                                           : "dc exact in nfa6", NULL);
            pcm_free(&fi);
            pcm_free(&fo);
        }
    }

    /* a one frame chunk is the seed and nothing else */
    {
        pcm_buf one, back;
        Err fe;
        memset(&one, 0, sizeof(one));
        memset(&back, 0, sizeof(back));
        one.count = 1;
        one.channels = 1;
        one.sample_rate = 44100;
        one.samples = (float*)malloc(sizeof(float));
        if (one.samples != NULL) {
            one.samples[0] = 0.5f;
            temp_path(path, sizeof(path), "n6o");
            fe = acpcm_encode6(path, &one, 16, 256, 2.0, 2.6);
            if (fe == ERR_OK)
                fe = acpcm_decode(path, &back);
            remove(path);
            check(fe == ERR_OK && back.count == 1 && same(&one, &back), verbose,
                  "nfa6 one frame is just the seed", NULL);
            pcm_free(&back);
        }
        pcm_free(&one);
    }

    /* the parameters are validated before any file is written */
    expect_err(acpcm_encode6(path, &in, 16, 0, 2.0, 2.6), ERR_RANGE, verbose,
               "nfa6 rejects block 0");
    expect_err(acpcm_encode6(path, &in, 16, 256, 1.0, 2.6), ERR_RANGE, verbose,
               "nfa6 rejects e <= 1");
    expect_err(acpcm_encode6(path, &in, 16, 256, 2.0, 0.0), ERR_RANGE, verbose,
               "nfa6 rejects rate 0");
    expect_err(acpcm_encode6(path, &in, 16, 256, 2.0, 65.0), ERR_RANGE, verbose,
               "nfa6 rejects rate above the maximum");
    expect_err(acpcm_encode6(path, &in, 0, 256, 2.0, 2.6), ERR_RANGE, verbose,
               "nfa6 rejects bits below the minimum");
    in.samples[7] = 0.0f / 0.0f;   /* NaN */
    expect_err(acpcm_encode6(path, &in, 16, 256, 2.0, 2.6), ERR_BAD_INPUT,
               verbose, "nfa6 rejects NaN samples");
    in.samples[7] = 0.0f;

    pcm_free(&in);
    pcm_free(&o6);
}



static void test_extract(int verbose)
{
    char path[512];
    char det[192];
    const size_t frames = 4096;
    const uint16_t ch = 2;

    pcm_buf in;
    memset(&in, 0, sizeof(in));
    in.count = frames * ch;
    in.channels = ch;
    in.sample_rate = 44100;
    in.samples = (float*)calloc(in.count, sizeof(float));
    if (in.samples == NULL)
        return;
    rng_seed(1234);
    for (size_t i = 0; i < in.count; i++) {
        double t = (double)(i / ch) / 44100.0;
        in.samples[i] = (float)(0.35 * sin(2.0 * 3.14159265358979 * 277.0 * t)) +
                        rng_next();
    }

    temp_path(path, sizeof(path), "codes");
    acpcm_codes c1;
    memset(&c1, 0, sizeof(c1));
    Err e = acpcm_encode(path, &in, 8);
    if (e == ERR_OK)
        e = acpcm_extract_codes(path, &c1);
    int ok = (e == ERR_OK && c1.channels == ch && c1.frames == frames &&
              c1.codes == frames - 1 && c1.q != NULL);
    snprintf(det, sizeof(det), "%llu codes x %u ch, one per frame after seed",
             (unsigned long long)(ok ? c1.codes : 0), c1.channels);
    check(ok, verbose, "nfa1 residual codes extract", det);

    /* the codes are the transmitted residual, so they must fit the quantiser */
    int inrange = 1;
    int nonzero = 0;
    if (ok) {
        int32_t lim = (int32_t)((1u << (8 - 1)) - 1u);
        for (size_t i = 0; i < c1.channels * c1.codes; i++) {
            if (c1.q[i] > lim - 1 || c1.q[i] < -lim)
                inrange = 0;
            if (c1.q[i] != 0)
                nonzero = 1;
        }
    }
    check(ok && inrange && nonzero, verbose, "codes are residual symbols",
          inrange ? "all within the quantiser range" : "out of range");

    /* NFA2 with refits disabled is bit identical to NFA1, so the codes match */
    acpcm_codes c2;
    memset(&c2, 0, sizeof(c2));
    int same_codes = 0;
    if (acpcm_encode2(path, &in, 8, 512, ACP2_NEVER) == ERR_OK &&
        acpcm_extract_codes(path, &c2) == ERR_OK) {
        same_codes = (c2.codes == c1.codes && c2.channels == c1.channels);
        for (size_t i = 0; same_codes && i < c1.channels * c1.codes; i++)
            if (c2.q[i] != c1.q[i])
                same_codes = 0;
    }
    check(same_codes, verbose, "nfa2 never codes match nfa1", "");

    /* a silent channel carries no residual at all */
    int silent_ok = 0;
    memset(in.samples, 0, in.count * sizeof(float));
    if (acpcm_encode(path, &in, 8) == ERR_OK) {
        acpcm_codes cs;
        if (acpcm_extract_codes(path, &cs) == ERR_OK) {
            silent_ok = (cs.codes == frames - 1);
            for (size_t i = 0; silent_ok && i < cs.channels * cs.codes; i++)
                if (cs.q[i] != 0)
                    silent_ok = 0;
            acpcm_codes_free(&cs);
        }
    }
    check(silent_ok, verbose, "silence extracts all zero codes", "");

    /* a one frame chunk is a seed and nothing else */
    pcm_buf one;
    memset(&one, 0, sizeof(one));
    one.count = 1;
    one.channels = 1;
    one.sample_rate = 44100;
    one.samples = (float*)calloc(1, sizeof(float));
    int one_ok = 0;
    if (one.samples != NULL && acpcm_encode(path, &one, 8) == ERR_OK) {
        acpcm_codes cz;
        if (acpcm_extract_codes(path, &cz) == ERR_OK) {
            one_ok = (cz.codes == 0 && cz.frames == 1 && cz.q == NULL);
            acpcm_codes_free(&cz);
        }
    }
    check(one_ok, verbose, "one frame yields no codes", "");
    pcm_free(&one);

    /* NFA3..NFA6 have no single residual symbol */
    int rej3 = 0, rej4 = 0, rej5 = 0, rej6 = 0;
    if (acpcm_encode3(path, &in, 8) == ERR_OK) {
        acpcm_codes cx;
        rej3 = (acpcm_extract_codes(path, &cx) == ERR_UNSUPPORTED_FMT);
        acpcm_codes_free(&cx);
    }
    if (acpcm_encode4_baskets(path, &in, 8, 32) == ERR_OK) {
        acpcm_codes cx4;
        rej4 = (acpcm_extract_codes(path, &cx4) == ERR_UNSUPPORTED_FMT);
        acpcm_codes_free(&cx4);
    }
    if (acpcm_encode5(path, &in, 8, 2.0, 0.01) == ERR_OK) {
        acpcm_codes cx1;
        rej5 = (acpcm_extract_codes(path, &cx1) == ERR_UNSUPPORTED_FMT);
        acpcm_codes_free(&cx1);
    }
    if (acpcm_encode6(path, &in, 8, 256, 2.0, 2.6) == ERR_OK) {
        acpcm_codes cx2;
        rej6 = (acpcm_extract_codes(path, &cx2) == ERR_UNSUPPORTED_FMT);
        acpcm_codes_free(&cx2);
    }
    if (0) {
        
    }
    check(rej3 && rej4 && rej5 && rej6, verbose,
          "extract rejects nfa3, nfa4, nfa5, nfa6", "");

    acpcm_codes_free(&c1);
    acpcm_codes_free(&c2);
    pcm_free(&in);
    remove(path);
}

Err acpcm_selftest(int verbose)
{
    failures = 0;
    printf("acpcm selftest\n");
    test_exact(verbose);
    test_widths(verbose);
    test_no_clipping(verbose);
    test_noise_bound(verbose);
    test_drift(verbose);
    test_baskets(verbose);
    test_nfa4(verbose);
    test_nfa5(verbose);
    test_nfa6(verbose);
    test_extract(verbose);
    test_rejects(verbose);
    printf("%s: %d failure(s)\n", failures ? "FAILED" : "passed", failures);
    return failures ? ERR_BAD_ARGS : ERR_OK;
}