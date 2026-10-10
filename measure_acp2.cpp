/* Scratch measurement harness for NFA1/NFA2 (acpcm) — rate, SNR, speed,
 * refit counts, meter tuning.  Not part of the Makefile build:
 *
 *   g++ -std=c++17 -O2 -Wall -Wextra -Werror -pedantic \
 *       -o measure_acp2 measure_acp2.cpp acpcm_inst.cpp nofft.cpp -lm
 *
 * acpcm_inst.cpp is a copy of acpcm.cpp with counters and a tunable basket
 * meter; codec behaviour is identical.
 */
#include "acpcm.h"
#include "nofft.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

extern int g_acp2_den;
extern int g_acp2_pct;
extern long g_acp2_updates;
extern long g_acp4_baskets;

static uint32_t rng_state;

static void rng_seed(uint32_t s)
{
    rng_state = s ? s : 1u;
}

static float rng_next(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return ((float)(int32_t)rng_state / 2147483648.0f) * 0.5f;
}

struct Res {
    int ok;
    double bps;
    double snr;
    double enc_ms;
    double dec_ms;
    long updates;
};

static double now_ms(void)
{
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>(
               clock::now().time_since_epoch()).count();
}

/* encode + decode through the real file path; best-of-reps timings */
static Res run(const pcm_buf* in, uint16_t bits, int v2, uint32_t bl,
               acpcm_p2_policy pol, int reps)
{
    char path[128];
    snprintf(path, sizeof(path), "acp2m_%d.acp", (int)getpid());

    Res r;
    memset(&r, 0, sizeof(r));
    double best_e = 1e30, best_d = 1e30;

    for (int rep = 0; rep < reps; rep++) {
        g_acp2_updates = 0;
        double t0 = now_ms();
        Err e = v2 ? acpcm_encode2(path, in, bits, bl, pol)
                   : acpcm_encode(path, in, bits);
        double t1 = now_ms();
        if (e != ERR_OK) {
            remove(path);
            return r;
        }
        if (t1 - t0 < best_e)
            best_e = t1 - t0;
        if (rep == 0)
            r.updates = g_acp2_updates;

        acpcm_info info;
        memset(&info, 0, sizeof(info));
        if (acpcm_file_info(path, &info) != ERR_OK || info.frames == 0) {
            remove(path);
            return r;
        }
        r.bps = 8.0 * (double)info.payload /
                ((double)info.frames * (double)info.channels);

        pcm_buf out;
        memset(&out, 0, sizeof(out));
        double t2 = now_ms();
        e = acpcm_decode(path, &out);
        double t3 = now_ms();
        if (e != ERR_OK) {
            remove(path);
            return r;
        }
        if (t3 - t2 < best_d)
            best_d = t3 - t2;
        if (rep == 0 && out.count == in->count && out.count > 0)
            r.snr = (double)nofft_decode_snr_db(in->samples, out.samples,
                                                out.count);
        pcm_free(&out);
    }

    remove(path);
    r.enc_ms = best_e;
    r.dec_ms = best_d;
    r.ok = 1;
    return r;
}

/* NFA3 path: same timing/SNR protocol, but acpcm_encode3 (no -block). */
static Res run3(const pcm_buf* in, uint16_t bits, int reps)
{
    char path[128];
    snprintf(path, sizeof(path), "acp2m3_%d.acp", (int)getpid());

    Res r;
    memset(&r, 0, sizeof(r));
    double best_e = 1e30, best_d = 1e30;

    for (int rep = 0; rep < reps; rep++) {
        double t0 = now_ms();
        Err e = acpcm_encode3(path, in, bits);
        double t1 = now_ms();
        if (e != ERR_OK) {
            remove(path);
            return r;
        }
        if (t1 - t0 < best_e)
            best_e = t1 - t0;

        acpcm_info info;
        memset(&info, 0, sizeof(info));
        if (acpcm_file_info(path, &info) != ERR_OK || info.frames == 0) {
            remove(path);
            return r;
        }
        r.bps = 8.0 * (double)info.payload /
                ((double)info.frames * (double)info.channels);

        pcm_buf out;
        memset(&out, 0, sizeof(out));
        double t2 = now_ms();
        e = acpcm_decode(path, &out);
        double t3 = now_ms();
        if (e != ERR_OK) {
            remove(path);
            return r;
        }
        if (t3 - t2 < best_d)
            best_d = t3 - t2;
        if (rep == 0 && out.count == in->count && out.count > 0)
            r.snr = (double)nofft_decode_snr_db(in->samples, out.samples,
                                                out.count);
        pcm_free(&out);
    }

    remove(path);
    r.enc_ms = best_e;
    r.dec_ms = best_d;
    r.ok = 1;
    return r;
}

/* NFA4 path: the basket-only codec.  The channel is shifted so its minimum
   sits at zero, then every sample is coded as one basket index on a grid of
   the shifted range; there is no predictor and no residual.  The bits argument
   is kept for the file header but does not affect the grid.  `grid` is either
   a step percent (run4) or a basket count (run4_baskets); the count path is the
   only way past the 101 rows an integer-percent step allows. */
static Res run4_grid(const pcm_buf* in, uint16_t bits, uint32_t grid,
                     int by_baskets, int reps)
{
    char path[128];
    snprintf(path, sizeof(path), "acp2m4_%d.acp", (int)getpid());

    Res r;
    memset(&r, 0, sizeof(r));
    double best_e = 1e30, best_d = 1e30;

    for (int rep = 0; rep < reps; rep++) {
        double t0 = now_ms();
        Err e = by_baskets ? acpcm_encode4_baskets(path, in, bits, grid)
                           : acpcm_encode4(path, in, bits, grid);
        double t1 = now_ms();
        if (e != ERR_OK) {
            remove(path);
            return r;
        }
        if (t1 - t0 < best_e)
            best_e = t1 - t0;

        acpcm_info info;
        memset(&info, 0, sizeof(info));
        if (acpcm_file_info(path, &info) != ERR_OK || info.frames == 0) {
            remove(path);
            return r;
        }
        r.bps = 8.0 * (double)info.payload /
                ((double)info.frames * (double)info.channels);

        pcm_buf out;
        memset(&out, 0, sizeof(out));
        double t2 = now_ms();
        e = acpcm_decode(path, &out);
        double t3 = now_ms();
        if (e != ERR_OK) {
            remove(path);
            return r;
        }
        if (t3 - t2 < best_d)
            best_d = t3 - t2;
        if (rep == 0 && out.count == in->count && out.count > 0)
            r.snr = (double)nofft_decode_snr_db(in->samples, out.samples,
                                                out.count);
        pcm_free(&out);
    }

    remove(path);
    r.enc_ms = best_e;
    r.dec_ms = best_d;
    r.ok = 1;
    return r;
}

/* step percent path (nb = 100/step + 1, at most 101 rows) */
static Res run4(const pcm_buf* in, uint16_t bits, uint32_t step, int reps)
{
    return run4_grid(in, bits, step, 0, reps);
}

/* direct basket count path, 1..ACPCM4_MAX_BUCKETS */
static Res run4_baskets(const pcm_buf* in, uint16_t bits, uint32_t baskets,
                        int reps)
{
    return run4_grid(in, bits, baskets, 1, reps);
}

static pcm_buf alloc_pcm(size_t frames, uint16_t ch, uint32_t rate)
{
    pcm_buf p;
    memset(&p, 0, sizeof(p));
    p.count = frames * (size_t)ch;
    p.channels = ch;
    p.sample_rate = rate;
    p.samples = (float*)calloc(p.count, sizeof(float));
    return p;
}

/* test_drift signal: low tone first half (a1 wants the clamp), white noise
   second half (a1 wants 0); one global fit has to compromise */
static pcm_buf sig_crafted(size_t frames)
{
    pcm_buf p = alloc_pcm(frames, 1, 44100);
    rng_seed(0xD1FFu);
    for (size_t i = 0; i < frames; i++) {
        if (i < frames / 2)
            p.samples[i] = (float)(0.4 * sin(2.0 * 3.14159265358979 *
                                             110.0 * (double)i / 44100.0));
        else
            p.samples[i] = 0.35f * rng_next();
    }
    return p;
}

/* stationary: a1 fit is already right, refits can only cost */
static pcm_buf sig_noise(size_t frames)
{
    pcm_buf p = alloc_pcm(frames, 1, 44100);
    rng_seed(0xBEEFu);
    for (size_t i = 0; i < frames; i++)
        p.samples[i] = 0.4f * rng_next();
    return p;
}

static pcm_buf sig_tone(size_t frames)
{
    pcm_buf p = alloc_pcm(frames, 1, 44100);
    for (size_t i = 0; i < frames; i++)
        p.samples[i] = (float)(0.4 * sin(2.0 * 3.14159265358979 *
                                         440.0 * (double)i / 44100.0));
    return p;
}

/* test_widths signal: tones plus a little noise */
static pcm_buf sig_mix(size_t frames)
{
    pcm_buf p = alloc_pcm(frames, 1, 44100);
    rng_seed(0x1234u);
    for (size_t i = 0; i < frames; i++) {
        double t = (double)i / 44100.0;
        p.samples[i] = (float)(0.35 * sin(t * 2.0 * 3.14159265358979 * 220.0) +
                               0.25 * sin(t * 2.0 * 3.14159265358979 * 1500.0) +
                               0.25 * (double)rng_next());
    }
    return p;
}

static void print_pair(const Res& r)
{
    if (r.ok)
        printf("%.4f/%.2f", r.bps, r.snr);
    else
        printf("ERR");
}

/* T0: must reproduce the selftest's test_drift numbers exactly */
static void table_sanity(void)
{
    pcm_buf in = sig_crafted(12000);
    Res v1 = run(&in, 6, 0, 0, ACP2_NEVER, 1);
    Res ne = run(&in, 6, 1, 1024, ACP2_NEVER, 1);
    Res al = run(&in, 6, 1, 1024, ACP2_ALWAYS, 1);
    Res dr = run(&in, 6, 1, 1024, ACP2_DRIFT, 1);

    printf("T0 sanity: instrumented build must equal the selftest's test_drift\n"
           "  (crafted 12000 frames, bits=6, block=1024; value = b/s / SNR dB;\n"
           "  expected b/s 4.4253 / 4.4267 / 3.5027 / 3.4973, NFA1 SNR 36.59;\n"
           "  never must be bit-identical to NFA1, so its SNR is the same):\n");
    printf("  NFA1      ");
    print_pair(v1);
    printf("   NFA2 never  ");
    print_pair(ne);
    printf("   NFA2 always ");
    print_pair(al);
    printf("   NFA2 drift  ");
    print_pair(dr);
    printf("\n  drift a1 refits=%ld\n\n", dr.updates);
    pcm_free(&in);
}

static void table_ladder(const pcm_buf* in)
{
    printf("T1 rate and fidelity vs quantiser width\n"
           "  crafted 10 s tone->noise (predictor must adapt), block=1024\n"
           "  b/s        payload bits divided by sample count; lower = smaller file\n"
           "  SNR dB     decode vs input; higher = closer to the original\n"
           "  a1 refits  how often the drift meter replaced the predictor gain\n"
           "  never      NFA2 reading boundary flags only; must track NFA1 exactly\n");
    printf("| bits (width) | NFA1 b/s | NFA1 SNR dB | NFA2 never b/s"
           " | NFA2 always b/s | NFA2 always SNR dB | NFA2 drift b/s"
           " | NFA2 drift SNR dB | NFA2 drift a1 refits |\n");
    printf("|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n");
    for (uint16_t bits = 2; bits <= 12; bits++) {
        Res v1 = run(in, bits, 0, 0, ACP2_NEVER, 1);
        Res ne = run(in, bits, 1, 1024, ACP2_NEVER, 1);
        Res al = run(in, bits, 1, 1024, ACP2_ALWAYS, 1);
        Res dr = run(in, bits, 1, 1024, ACP2_DRIFT, 1);
        printf("| %u | %.4f | %.2f | %.4f | %.4f | %.2f | %.4f | %.2f | %ld |\n",
               bits, v1.bps, v1.snr, ne.bps, al.bps, al.snr,
               dr.bps, dr.snr, dr.updates);
    }
    printf("\n");
}

static void table_signals(const pcm_buf* sigs[], const char* names[],
                          size_t n)
{
    printf("T2 signal survey, bits=6, block=1024\n"
           "  b/s     payload bits per sample; lower = smaller file at this quality\n"
           "  SNR dB  decode fidelity vs input; higher = better\n"
           "  NFA2 never decodes bit-identical to NFA1, so it has no SNR column\n");
    printf("| signal | frames x ch | NFA1 b/s | NFA1 SNR dB | NFA2 never b/s"
           " | NFA2 always b/s | NFA2 always SNR dB | NFA2 drift b/s"
           " | NFA2 drift SNR dB |\n");
    printf("|---|---:|---:|---:|---:|---:|---:|---:|---:|\n");
    for (size_t k = 0; k < n; k++) {
        const pcm_buf* s = sigs[k];
        Res v1 = run(s, 6, 0, 0, ACP2_NEVER, 1);
        Res ne = run(s, 6, 1, 1024, ACP2_NEVER, 1);
        Res al = run(s, 6, 1, 1024, ACP2_ALWAYS, 1);
        Res dr = run(s, 6, 1, 1024, ACP2_DRIFT, 1);
        printf("| %s | %zux%u | %.4f | %.2f | %.4f | %.4f | %.2f | %.4f | %.2f |\n",
               names[k], s->count / s->channels, s->channels,
               v1.bps, v1.snr, ne.bps, al.bps, al.snr, dr.bps, dr.snr);
    }
    printf("\n");
}

static void table_block_sweep(const pcm_buf* in, double v1_bps)
{
    static const uint32_t blocks[] = { 128, 256, 512, 1024, 2048, 4096, 8192 };
    printf("T3 refit-window size sweep, crafted, bits=6\n"
           "  NFA1 baseline without refits: %.4f b/s\n"
           "  block (frames)  frames between refit opportunities\n"
           "  refits          a1 replacements actually performed\n"
           "  drift - NFA1    rate change vs baseline in b/s; negative = saves\n",
           v1_bps);
    printf("| block (frames) | NFA2 always b/s | NFA2 always refits"
           " | NFA2 drift b/s | NFA2 drift SNR dB | NFA2 drift refits"
           " | drift - NFA1 b/s |\n");
    printf("|---:|---:|---:|---:|---:|---:|---:|\n");
    for (size_t i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++) {
        Res al = run(in, 6, 1, blocks[i], ACP2_ALWAYS, 1);
        Res dr = run(in, 6, 1, blocks[i], ACP2_DRIFT, 1);
        printf("| %u | %.4f | %ld | %.4f | %.2f | %ld | %+.4f |\n",
               blocks[i], al.bps, al.updates, dr.bps, dr.snr, dr.updates,
               dr.bps - v1_bps);
    }
    printf("\n");
}

static void table_meter(const pcm_buf* in, double drift_bps, long drift_upd)
{
    static const int dens[] = { 4, 8, 16, 32 };
    static const int pcts[] = { 1, 2, 5 };

    printf("T4 basket-meter sensitivity, crafted, bits=6, block=1024\n"
           "  den  a sample counts as expensive when |q| >= lim/den, i.e. den=8\n"
           "       means its magnitude costs at least 1/8 of the symbol range\n"
           "  pct  percent of a block's samples that must be expensive before\n"
           "       the encoder fires a refit at the next boundary\n"
           "  cell b/s (a1 refits); defaults den=8 pct=1: %.4f b/s, %ld refits\n"
           "  NFA1 baseline without refits: see T3\n",
           drift_bps, drift_upd);
    printf("| den (expensive threshold) | pct 1%% | pct 2%% | pct 5%% |\n");
    printf("|---:|---:|---:|---:|\n");
    for (size_t i = 0; i < sizeof(dens) / sizeof(dens[0]); i++) {
        printf("| %d ", dens[i]);
        for (size_t j = 0; j < sizeof(pcts) / sizeof(pcts[0]); j++) {
            g_acp2_den = dens[i];
            g_acp2_pct = pcts[j];
            Res dr = run(in, 6, 1, 1024, ACP2_DRIFT, 1);
            printf("| %.4f (%ld) ", dr.bps, dr.updates);
        }
        printf("|\n");
    }
    g_acp2_den = 8;
    g_acp2_pct = 1;
    printf("\n");
}

static void table_nfa3_ladder(const pcm_buf* in)
{
    printf("T5 NFA3 width ladder, crafted 10 s tone->noise, one predictor\n"
           "  NFA3 replaces the NFA1 unary run with a basket index coded from an\n"
           "  adaptive histogram, then the residual mantissa bits; a saturated\n"
           "  residual escapes to a raw sample.  NFA1 and NFA3 both skip refits.\n"
           "  delta b/s  NFA3 minus NFA1; negative means the table paid off\n");
    printf("| bits | NFA1 b/s | NFA1 SNR dB | NFA3 b/s | NFA3 SNR dB"
           " | NFA3-NFA1 b/s |\n");
    printf("|---:|---:|---:|---:|---:|---:|\n");
    for (uint16_t bits = 2; bits <= 12; bits++) {
        Res v1 = run(in, bits, 0, 0, ACP2_NEVER, 1);
        Res v3 = run3(in, bits, 1);
        printf("| %u | %.4f | %.2f | %.4f | %.2f | %+.4f |\n",
               bits, v1.bps, v1.snr, v3.bps, v3.snr, v3.bps - v1.bps);
    }
    printf("\n");
}

static void table_nfa3(const pcm_buf* sigs[], const char* names[], size_t n)
{
    printf("T6 NFA3 signal survey, bits=6, one predictor (no refits)\n"
           "  delta b/s  NFA3 minus NFA1; negative means the table paid off\n");
    printf("| signal | frames x ch | NFA1 b/s | NFA1 SNR dB | NFA3 b/s"
           " | NFA3 SNR dB | NFA3-NFA1 b/s |\n");
    printf("|---|---:|---:|---:|---:|---:|---:|\n");
    for (size_t k = 0; k < n; k++) {
        Res v1 = run(sigs[k], 6, 0, 0, ACP2_NEVER, 1);
        Res v3 = run3(sigs[k], 6, 1);
        printf("| %s | %zux%u | %.4f | %.2f | %.4f | %.2f | %+.4f |\n",
               names[k], sigs[k]->count / sigs[k]->channels, sigs[k]->channels,
               v1.bps, v1.snr, v3.bps, v3.snr, v3.bps - v1.bps);
    }
    printf("\n");
}

static void table_nfa4_ladder(const pcm_buf* in)
{
    printf("T8 NFA4 width ladder, crafted 10 s tone->noise, step=5 percent\n"
           "  NFA4 is basket-only: one basket index per sample on a step-percent\n"
           "  grid of the shifted channel range.  It ignores the residual width,\n"
           "  so the NFA4 columns stay flat down the bits column while NFA1/NFA3\n"
           "  get finer.  Step is the bucket width in percent of channel range.\n");
    printf("| bits | NFA1 b/s | NFA3 b/s | NFA4 b/s | NFA4 SNR dB"
           " | NFA4-NFA3 b/s |\n");
    printf("|---:|---:|---:|---:|---:|---:|\n");
    for (uint16_t bits = 2; bits <= 12; bits++) {
        Res v1 = run(in, bits, 0, 0, ACP2_NEVER, 1);
        Res v3 = run3(in, bits, 1);
        Res v4 = run4(in, bits, 5, 1);
        printf("| %u | %.4f | %.4f | %.4f | %.2f | %+.4f |\n",
               bits, v1.bps, v3.bps, v4.bps, v4.snr, v4.bps - v3.bps);
    }
    printf("\n");
}

static void table_nfa4(const pcm_buf* sigs[], const char* names[], size_t n)
{
    printf("T9 NFA4 signal survey, bits=8, step=5 percent\n"
           "  NFA4 is basket-only (no predictor, no residual); NFA3 is the\n"
           "  predictive codec at the same residual width.  NFA4 trades the\n"
           "  whole bitstream for one basket symbol per sample, so at a good step\n"
           "  it is far below NFA3 in rate but also lower in SNR.\n");
    printf("| signal | frames x ch | NFA3 b/s | NFA4 b/s | NFA4 SNR dB"
           " | NFA4-NFA3 b/s |\n");
    printf("|---|---:|---:|---:|---:|---:|\n");
    for (size_t k = 0; k < n; k++) {
        Res v3 = run3(sigs[k], 8, 1);
        Res v4 = run4(sigs[k], 8, 5, 1);
        printf("| %s | %zux%u | %.4f | %.4f | %.2f | %+.4f |\n",
               names[k], sigs[k]->count / sigs[k]->channels, sigs[k]->channels,
               v3.bps, v4.bps, v4.snr, v4.bps - v3.bps);
    }
    printf("\n");
}

static void table_nfa4_baskets(const pcm_buf* sigs[], const char* names[],
                               size_t n)
{
    /* Exact basket counts, including the 170 and 256 grids the integer-percent
       step cannot express. */
    static const uint32_t want[] = { 2, 3, 4, 5, 6, 8, 11, 16, 21, 26, 34,
                                     51, 101, 170, 256 };
    printf("T10 NFA4 rate and fidelity by basket count, bits=8\n"
           "  baskets is the grid alphabet size nb, stored in the header, so the\n"
           "  per-sample basket symbol costs about log2(baskets) bits and the\n"
           "  grid is exact (no integer-percent cap at 101 rows).  Every channel,\n"
           "  flat or not, uses the full alphabet (the count is not data\n"
           "  dependent).\n");
    for (size_t k = 0; k < n; k++) {
        printf("  %s (%zux%u)\n", names[k],
               sigs[k]->count / sigs[k]->channels, sigs[k]->channels);
        printf("| baskets | NFA4 b/s | NFA4 SNR dB |\n"
               "|---:|---:|---:|\n");
        for (size_t j = 0; j < sizeof(want) / sizeof(want[0]); j++) {
            Res v4 = run4_baskets(sigs[k], 8, want[j], 1);
            printf("| %u | %.4f | %.2f |\n", want[j], v4.bps, v4.snr);
        }
    }
    printf("\n");
}

static void table_speed(const pcm_buf* in, const char* name)
{
    double mframes = (double)(in->count / in->channels) / 1e6;
    printf("T7 encode/decode wall time, %s (%zu frames x %u ch = %.3f M frames),\n"
           "  bits=6, block=1024, best of 5 runs; wall time includes writing or\n"
           "  reading the temporary file, throughput is M frames/s (frames, not\n"
           "  samples: stereo counts one frame per L+R pair)\n",
           name, in->count / in->channels, in->channels, mframes);
    printf("| variant | encode ms | encode M frames/s | decode ms"
           " | decode M frames/s |\n");
    printf("|---|---:|---:|---:|---:|\n");
    static const struct {
        const char* name;
        int v2;
        int v3;
        int v4;
        acpcm_p2_policy pol;
    } vars[] = {
        { "NFA1 baseline",        0, 0, 0, ACP2_NEVER },
        { "NFA2 never (flags)",   1, 0, 0, ACP2_NEVER },
        { "NFA2 always (refit)",  1, 0, 0, ACP2_ALWAYS },
        { "NFA2 drift (meter)",   1, 0, 0, ACP2_DRIFT },
        { "NFA3 baskets",         0, 1, 0, ACP2_NEVER },
        { "NFA4 baskets",         0, 0, 1, ACP2_NEVER },
    };
    for (size_t i = 0; i < sizeof(vars) / sizeof(vars[0]); i++) {
        Res r = vars[i].v3 ? run3(in, 6, 5)
                           : vars[i].v4 ? run4(in, 6, 5, 5)
                                        : run(in, 6, vars[i].v2, 1024,
                                              vars[i].pol, 5);
        printf("| %s | %.2f | %.2f | %.2f | %.2f |\n",
               vars[i].name, r.enc_ms,
               (double)(in->count / in->channels) / (r.enc_ms / 1000.0) / 1e6,
               r.dec_ms,
               (double)(in->count / in->channels) / (r.dec_ms / 1000.0) / 1e6);
    }
    printf("\n");
}

int main(int argc, char** argv)
{
    if (argc > 1 && strcmp(argv[1], "nfa4-baskets") == 0) {
        pcm_buf c = sig_crafted(441000);
        pcm_buf o, o1;
        memset(&o, 0, sizeof(o));
        memset(&o1, 0, sizeof(o1));
        int ho = (wav_load("orig.wav", &o) == ERR_OK);
        int ho1 = (wav_load("orig1.wav", &o1) == ERR_OK);
        const pcm_buf* s[4];
        const char* nm[4];
        size_t ns = 0;
        if (c.samples != NULL) { s[ns] = &c; nm[ns++] = "crafted tone->noise"; }
        if (ho) { s[ns] = &o; nm[ns++] = "orig.wav"; }
        if (ho1) { s[ns] = &o1; nm[ns++] = "orig1.wav"; }
        table_nfa4_baskets(s, nm, ns);
        pcm_free(&c);
        if (ho) pcm_free(&o);
        if (ho1) pcm_free(&o1);
        return 0;
    }

    printf("acpcm NFA2 measurement\n"
           "  NFA1 = existing format, one global predictor for the whole file\n"
           "  NFA2 = new format, predictor gain a1 can be replaced at block edges\n"
           "         never  = refits disabled; audio must match NFA1 bit for bit\n"
           "         always = refit at every block boundary\n"
           "         drift  = refit only when the basket meter reports drift\n"
           "  b/s    bits per sample of the file payload; lower = better rate\n"
           "  SNR dB fidelity of decode vs input; higher = better\n\n");

    table_sanity();

    pcm_buf crafted = sig_crafted(441000);
    if (crafted.samples == NULL)
        return 1;
    table_ladder(&crafted);

    pcm_buf noise = sig_noise(441000);
    pcm_buf tone = sig_tone(441000);
    pcm_buf mix = sig_mix(441000);
    pcm_buf orig;
    memset(&orig, 0, sizeof(orig));
    int has_orig = (wav_load("orig.wav", &orig) == ERR_OK);

    pcm_buf extra[3];
    const char* extra_names[3] = { "orig1.wav", "orig16.wav",
                                   "listen/20_nofade_L32_D6_f16.16.wav" };
    int have_extra[3] = { 0, 0, 0 };

    const pcm_buf* sigs[8];
    const char* names[8];
    size_t nsig = 0;
    sigs[nsig] = &crafted; names[nsig++] = "crafted tone->noise";
    sigs[nsig] = &noise;    names[nsig++] = "stationary noise";
    sigs[nsig] = &tone;     names[nsig++] = "stationary tone 440";
    sigs[nsig] = &mix;      names[nsig++] = "tones + noise";
    if (has_orig) {
        sigs[nsig] = &orig;
        names[nsig++] = "orig.wav";
    }
    for (int i = 0; i < 3; i++) {
        memset(&extra[i], 0, sizeof(extra[i]));
        if (access(extra_names[i], R_OK) == 0 &&
            wav_load(extra_names[i], &extra[i]) == ERR_OK &&
            extra[i].count > 0) {
            have_extra[i] = 1;
            sigs[nsig] = &extra[i];
            names[nsig++] = extra_names[i];
        }
    }
    table_signals(sigs, names, nsig);
    table_nfa3_ladder(&crafted);
    table_nfa3(sigs, names, nsig);
    table_nfa4_ladder(&crafted);
    table_nfa4(sigs, names, nsig);

    Res v1 = run(&crafted, 6, 0, 0, ACP2_NEVER, 1);
    Res dr = run(&crafted, 6, 1, 1024, ACP2_DRIFT, 1);
    table_block_sweep(&crafted, v1.bps);
    table_meter(&crafted, dr.bps, dr.updates);

    table_speed(&crafted, "crafted");
    if (has_orig)
        table_speed(&orig, "orig.wav");

    pcm_free(&crafted);
    pcm_free(&noise);
    pcm_free(&tone);
    pcm_free(&mix);
    if (has_orig)
        pcm_free(&orig);
    for (int i = 0; i < 3; i++)
        if (have_extra[i])
            pcm_free(&extra[i]);
    return 0;
}
