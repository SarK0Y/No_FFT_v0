#define DR_WAV_IMPLEMENTATION
#include "third_party/dr_wav.h"

#include "nofft.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

static const char* err_str_impl(Err e)
{
    switch (e) {
    case ERR_OK:              return "ok";
    case ERR_IO:              return "i/o error";
    case ERR_OPEN:            return "cannot open";
    case ERR_NOT_WAV:         return "not a wav file";
    case ERR_UNSUPPORTED_FMT: return "unsupported sample format";
    case ERR_NO_MEMORY:       return "out of memory";
    case ERR_BAD_NOFFT:       return "corrupt no_fft file";
    case ERR_BAD_ARGS:        return "bad arguments";
    default:                  return "unknown error";
    }
}

err_str_fn g_err_str = err_str_impl;

typedef size_t (*pcm_read_fn)(drwav* wav, uint64_t frames, float* out);

static size_t pcm_read_f32(drwav* wav, uint64_t frames, float* out)
{
    return (size_t)drwav_read_pcm_frames_f32(wav, frames, out);
}

static pcm_read_fn g_pcm_read = pcm_read_f32;

static int wav_is_container(const unsigned char* magic)
{
    static const char* const known[] = { "RIFF", "RIFX", "riff", "RF64", "FORM" };

    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); ++i) {
        if (memcmp(magic, known[i], 4) == 0)
            return 1;
    }
    return 0;
}

void pcm_free(pcm_buf* p)
{
    if (p == NULL)
        return;
    free(p->samples);
    p->samples = NULL;
    p->count = 0;
}

Err wav_load(const char* path, pcm_buf* out)
{
    drwav wav;
    unsigned char magic[4];
    FILE* f;

    memset(out, 0, sizeof(*out));

    f = fopen(path, "rb");
    if (f == NULL)
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

    if (wav.channels == 0) {
        drwav_uninit(&wav);
        return ERR_BAD_NOFFT;
    }

    {
        uint64_t total = wav.totalPCMFrameCount * wav.channels;
        size_t done = 0;

        if (total == 0 || total > (uint64_t)((SIZE_MAX / sizeof(float)) - 1)) {
            drwav_uninit(&wav);
            return ERR_NO_MEMORY;
        }

        out->samples = (float*)malloc((size_t)total * sizeof(float));
        if (out->samples == NULL) {
            drwav_uninit(&wav);
            return ERR_NO_MEMORY;
        }

        while (done < (size_t)total) {
            size_t want = (size_t)total - done;
            size_t got;

            if (want > 4096)
                want = 4096;

            got = g_pcm_read(&wav, (uint64_t)want, out->samples + done);
            if (got == 0)
                break;

            done += got;
        }

        drwav_uninit(&wav);

        out->count = done;
        out->sample_rate = wav.sampleRate;
        out->channels = wav.channels;
        return ERR_OK;
    }
}

Err wav_save(const char* path, const pcm_buf* p)
{
    drwav wav;
    drwav_data_format fmt;
    FILE* f;

    if (p->samples == NULL || p->channels == 0 || p->sample_rate == 0)
        return ERR_BAD_ARGS;

    memset(&fmt, 0, sizeof(fmt));
    fmt.container = drwav_container_riff;
    fmt.format = DR_WAVE_FORMAT_IEEE_FLOAT;
    fmt.channels = p->channels;
    fmt.sampleRate = p->sample_rate;
    fmt.bitsPerSample = 32;

    f = fopen(path, "wb");
    if (f == NULL)
        return ERR_OPEN;
    fclose(f);

    if (!drwav_init_file_write(&wav, path, &fmt, nullptr))
        return ERR_OPEN;

    if (drwav_write_pcm_frames(&wav, (drwav_uint64)p->count, p->samples) != (drwav_uint64)p->count) {
        drwav_uninit(&wav);
        return ERR_IO;
    }

    drwav_uninit(&wav);
    return ERR_OK;
}

uint16_t nofft_effective_degree(uint16_t degree, uint32_t len)
{
    if (len <= 1)
        return 0;
    if ((uint32_t)degree > len - 1)
        return (uint16_t)(len - 1);
    return degree;
}

double nofft_subrange_mean(const float* s, size_t n)
{
    double acc = 0.0;
    size_t i;

    if (n == 0)
        return 0.0;

    for (i = 0; i < n; ++i)
        acc += (double)s[i];

    return acc / (double)n;
}

void nofft_subrange_pick(const float* s, size_t n, double mean, uint32_t* out_index, double* out_value)
{
    size_t i;
    size_t best = 0;
    double best_d = 0.0;

    if (n == 0) {
        *out_index = 0;
        *out_value = 0.0;
        return;
    }

    best_d = fabs((double)s[0] - mean);

    for (i = 1; i < n; ++i) {
        double d = fabs((double)s[i] - mean);
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }

    *out_index = (uint32_t)best;
    *out_value = (double)s[best];
}

Err nofft_solve_vandermonde(const double* xs, const double* ys, int n, double* out_coefs)
{
    double a[64 * 64];
    double rhs[64];
    int c, r, k;

    if (n < 1 || n > 64)
        return ERR_BAD_ARGS;

    for (r = 0; r < n; ++r) {
        double p = 1.0;
        for (c = 0; c < n; ++c) {
            a[r * n + c] = p;
            p *= xs[r];
        }
        rhs[r] = ys[r];
    }

    for (c = 0; c < n; ++c) {
        int piv = c;
        for (r = c + 1; r < n; ++r) {
            double av = fabs(a[r * n + c]);
            double pv = fabs(a[piv * n + c]);
            if (av > pv)
                piv = r;
        }
        if (fabs(a[piv * n + c]) < 1e-300)
            return ERR_BAD_NOFFT;
        if (piv != c) {
            for (k = 0; k < n; ++k) {
                double t = a[c * n + k];
                a[c * n + k] = a[piv * n + k];
                a[piv * n + k] = t;
            }
            {
                double t = rhs[c];
                rhs[c] = rhs[piv];
                rhs[piv] = t;
            }
        }
        for (r = c + 1; r < n; ++r) {
            double f = a[r * n + c] / a[c * n + c];
            if (f == 0.0)
                continue;
            for (k = c; k < n; ++k)
                a[r * n + k] -= f * a[c * n + k];
            rhs[r] -= f * rhs[c];
        }
    }

    for (r = n - 1; r >= 0; --r) {
        double s = rhs[r];
        for (k = r + 1; k < n; ++k)
            s -= a[r * n + k] * out_coefs[k];
        out_coefs[r] = s / a[r * n + r];
    }

    return ERR_OK;
}

double nofft_eval_poly(const double* c, int n, double x)
{
    double v = c[n - 1];
    int i;

    for (i = n - 2; i >= 0; --i)
        v = v * x + c[i];

    return v;
}

static void frame_constraints(const float* s, uint32_t len, uint16_t d, double* xs, double* ys)
{
    uint32_t nsub = (uint32_t)d + 1;
    uint32_t i;

    for (i = 0; i < nsub; ++i) {
        uint32_t start = ((uint64_t)i * len) / nsub;
        uint32_t end = ((uint64_t)(i + 1) * len) / nsub;
        uint32_t pick;
        uint32_t pos;
        double mean, val;

        if (end > len)
            end = len;
        if (end <= start)
            end = (start < len) ? start + 1 : len;

        mean = nofft_subrange_mean(s + start, (size_t)(end - start));
        nofft_subrange_pick(s + start, (size_t)(end - start), mean, &pick, &val);

        pos = start + pick;

        xs[i] = (len > 1) ? (2.0 * (double)pos / (double)(len - 1) - 1.0) : 0.0;
        ys[i] = (double)s[pos];
    }
}

Err nofft_encode(const char* path, const pcm_buf* p, uint32_t frame_len, uint16_t degree)
{
    FILE* f;
    uint64_t total_frames;
    uint64_t fi;
    uint8_t hdr[28];
    uint16_t ch = p->channels;

    if (p->samples == NULL || ch == 0 || frame_len == 0 || degree == 0)
        return ERR_BAD_ARGS;

    total_frames = (uint64_t)(p->count / ch);

    f = fopen(path, "wb");
    if (f == NULL)
        return ERR_OPEN;

    memcpy(hdr, NOFFT_MAGIC, 4);
    {
        uint32_t v = NOFFT_VERSION;
        memcpy(hdr + 4, &v, 4);
        memcpy(hdr + 8, &p->sample_rate, 4);
        memcpy(hdr + 12, &ch, 2);
        memcpy(hdr + 14, &degree, 2);
        memcpy(hdr + 16, &frame_len, 4);
    }
    if (fwrite(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fclose(f);
        return ERR_IO;
    }

    for (fi = 0; fi < total_frames; fi += frame_len) {
        uint32_t len = (uint32_t)((total_frames - fi < frame_len)
                                  ? (total_frames - fi) : frame_len);
        uint16_t d = nofft_effective_degree(degree, len);
        int n = (int)d + 1;
        double xs[64], ys[64], coefs[64];
        uint32_t c16;
        uint8_t flen[4];
        uint32_t written = 0;
        uint16_t ci;

        if (n > 64)
            n = 64;

        memcpy(&flen, &len, 4);
        if (fwrite(flen, 1, 4, f) != 4) {
            fclose(f);
            return ERR_IO;
        }

        for (ci = 0; ci < ch; ++ci) {
            Err e;
            int j;

            frame_constraints(p->samples + (fi * ch) + ci, len, d, xs, ys);
            e = nofft_solve_vandermonde(xs, ys, n, coefs);
            if (e != ERR_OK) {
                fclose(f);
                return e;
            }

            for (j = 0; j < n; ++j) {
                float v = (float)coefs[j];
                if (fwrite(&v, 1, sizeof(v), f) != sizeof(v)) {
                    fclose(f);
                    return ERR_IO;
                }
                written += 4;
            }
        }

        (void)written;
        (void)c16;
    }

    fclose(f);
    return ERR_OK;
}

Err nofft_decode(const char* path, pcm_buf* out)
{
    FILE* f;
    uint8_t hdr[28];
    uint32_t frame_len;
    uint16_t degree, ch;
    uint32_t sr;
    uint64_t done_frames = 0;
    size_t cap, used = 0;
    float* buf;
    uint8_t chunk[4];

    memset(out, 0, sizeof(*out));

    f = fopen(path, "rb");
    if (f == NULL)
        return ERR_OPEN;

    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fclose(f);
        return ERR_BAD_NOFFT;
    }
    if (memcmp(hdr, NOFFT_MAGIC, 4) != 0) {
        fclose(f);
        return ERR_BAD_NOFFT;
    }

    memcpy(&sr, hdr + 8, 4);
    memcpy(&ch, hdr + 12, 2);
    memcpy(&degree, hdr + 14, 2);
    memcpy(&frame_len, hdr + 16, 4);

    if (ch == 0 || frame_len == 0 || degree == 0) {
        fclose(f);
        return ERR_BAD_NOFFT;
    }

    cap = 1024 * (size_t)frame_len * ch;
    buf = (float*)malloc(cap * sizeof(float));
    if (buf == NULL) {
        fclose(f);
        return ERR_NO_MEMORY;
    }

    for (;;) {
        uint32_t len;
        uint16_t d;
        int n, i;
        uint16_t ci;
        double* coefs;

        if (fread(chunk, 1, 4, f) != 4)
            break;

        memcpy(&len, chunk, 4);
        if (len == 0 || len > frame_len) {
            free(buf);
            fclose(f);
            return ERR_BAD_NOFFT;
        }

        d = nofft_effective_degree(degree, len);
        n = (int)d + 1;

        if (used + (size_t)len * ch > cap) {
            size_t ncap = cap * 2 + (size_t)len * ch;
            float* nb = (float*)realloc(buf, ncap * sizeof(float));
            if (nb == NULL) {
                free(buf);
                fclose(f);
                return ERR_NO_MEMORY;
            }
            buf = nb;
            cap = ncap;
        }

        coefs = (double*)malloc(sizeof(double) * (size_t)n * ch);
        if (coefs == NULL) {
            free(buf);
            fclose(f);
            return ERR_NO_MEMORY;
        }

        for (ci = 0; ci < ch; ++ci) {
            for (i = 0; i < n; ++i) {
                float v;
                if (fread(&v, 1, sizeof(v), f) != sizeof(v)) {
                    free(coefs);
                    free(buf);
                    fclose(f);
                    return ERR_BAD_NOFFT;
                }
                coefs[(size_t)ci * n + i] = (double)v;
            }
        }

        for (ci = 0; ci < ch; ++ci) {
            size_t k;

            for (k = 0; k < len; ++k) {
                double t = (len > 1) ? (2.0 * (double)k / (double)(len - 1) - 1.0) : 0.0;
                buf[used + (size_t)k * ch + ci] =
                    (float)nofft_eval_poly(coefs + (size_t)ci * n, n, t);
            }
        }

        used += (size_t)len * ch;
        done_frames += len;
        free(coefs);
    }

    fclose(f);

    out->samples = buf;
    out->count = used;
    out->sample_rate = sr;
    out->channels = ch;
    (void)done_frames;
    return ERR_OK;
}

float nofft_decode_snr_db(const float* a, const float* b, size_t n)
{
    double sig = 0.0, err = 0.0;
    size_t i;

    if (n == 0)
        return 0.0f;

    for (i = 0; i < n; ++i) {
        double d = (double)a[i];
        double e = (double)a[i] - (double)b[i];
        sig += d * d;
        err += e * e;
    }

    if (err <= 0.0)
        return 999.0f;
    if (sig <= 0.0)
        return 0.0f;

    return (float)(10.0 * log10(sig / err));
}