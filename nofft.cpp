#define DR_WAV_IMPLEMENTATION
#include "third_party/dr_wav.h"

#include "nofft.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

typedef char nofft_magic_size_check[(sizeof(NOFFT_MAGIC) == 5) ? 1 : -1];
typedef char nofft_header_size_check[(NOFFT_HEADER_SIZE >= 29) ? 1 : -1];

static uint16_t f32_to_f16_bits(float f)
{
    uint32_t x;
    uint32_t sign, mant;
    int32_t e;

    memcpy(&x, &f, 4);
    sign = (x >> 16) & 0x8000u;
    e = (int32_t)((x >> 23) & 0xFFu);

    if (e == 0xFF) {
        if ((x & 0x007FFFFFu) != 0u)
            return (uint16_t)(sign | 0x7E00u); /* NaN */
        return (uint16_t)(sign | 0x7C00u);     /* Inf */
    }

    e = e - 127 + 15;
    mant = x & 0x007FFFFFu;

    if (e >= 31)
        return (uint16_t)(sign | 0x7BFFu);     /* saturate, not Inf */
    if (e > 0) {
        uint32_t m = mant >> 13;
        uint32_t half = 1u << 12;
        if ((mant & half) != 0u) {
            uint32_t lower = mant & (half - 1u);
            if (lower != 0u || (m & 1u) != 0u) {
                m += 1u;
                if (m == 0x400u) {
                    m = 0u;
                    e += 1;
                    if (e >= 31)
                        return (uint16_t)(sign | 0x7BFFu);
                }
            }
        }
        return (uint16_t)(sign | ((uint32_t)e << 10) | m);
    }

    if (e < -10)
        return (uint16_t)sign;                /* underflow to signed zero */

    mant |= 0x00800000u;
    {
        int32_t shift = 14 - e;
        uint32_t m = mant >> shift;
        uint32_t half = 1u << (shift - 1);
        if ((mant & half) != 0u) {
            uint32_t lower = mant & (half - 1u);
            if (lower != 0u || (m & 1u) != 0u)
                m += 1u;
        }
        return (uint16_t)(sign | m);
    }
}

static float f16_bits_to_f32(uint16_t h)
{
    uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    uint32_t e = (uint32_t)(h >> 10) & 0x1Fu;
    uint32_t mant = (uint32_t)(h & 0x03FFu);
    uint32_t out;
    float f;

    if (e == 0u) {
        if (mant == 0u) {
            out = sign;
        } else {
            int32_t k = 0;
            while ((mant & 0x400u) == 0u) {
                mant <<= 1;
                k += 1;
            }
            mant &= 0x03FFu;
            out = sign | ((uint32_t)(113 - k) << 23) | (mant << 13);
        }
    } else if (e == 31u) {
        out = sign | 0x7F800000u | (mant << 13);
    } else {
        out = sign | ((uint32_t)(e + 112u) << 23) | (mant << 13);
    }

    memcpy(&f, &out, 4);
    return f;
}

const char* nofft_coeff_format_str(nofft_coeff_format cf)
{
    return (cf == NOFFT_COEF_F16) ? "f16" : "f32";
}

int nofft_coeff_format_parse(const char* s, nofft_coeff_format* out)
{
    if (s == NULL)
        return 0;
    if (strcmp(s, "f16") == 0 || strcmp(s, "f16le") == 0 ||
        strcmp(s, "float16") == 0 || strcmp(s, "half") == 0) {
        *out = NOFFT_COEF_F16;
        return 1;
    }
    if (strcmp(s, "f32") == 0 || strcmp(s, "f32le") == 0 ||
        strcmp(s, "float32") == 0 || strcmp(s, "float") == 0) {
        *out = NOFFT_COEF_F32;
        return 1;
    }
    return 0;
}

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
    case ERR_RANGE:           return "coefficient out of range for the requested format (try f32)";
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
        uint32_t ch = wav.channels;
        uint64_t total = wav.totalPCMFrameCount * (uint64_t)ch;
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

        out->sample_rate = wav.sampleRate;
        out->channels = ch;

        while (done < (size_t)total) {
            uint64_t want = ((uint64_t)total - (uint64_t)done) / (uint64_t)ch;
            size_t got;

            if (want > 4096)
                want = 4096;

            got = g_pcm_read(&wav, want, out->samples + done);
            if (got == 0)
                break;

            done += got * (size_t)ch;
        }

        drwav_uninit(&wav);

        out->count = done;
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

    {
        uint64_t frames = (uint64_t)p->count / (uint64_t)p->channels;

        if (drwav_write_pcm_frames(&wav, frames, p->samples) != frames) {
            drwav_uninit(&wav);
            return ERR_IO;
        }
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

double nofft_subrange_mean(const float* s, size_t n, size_t stride)
{
    double acc = 0.0;
    size_t i;

    if (n == 0)
        return 0.0;

    for (i = 0; i < n; ++i)
        acc += (double)s[i * stride];

    return acc / (double)n;
}

void nofft_subrange_pick(const float* s, size_t n, size_t stride, double mean, uint32_t* out_index, double* out_value)
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
        double d = fabs((double)s[i * stride] - mean);
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }

    *out_index = (uint32_t)best;
    *out_value = (double)s[best * stride];
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

static void frame_constraints(const float* s, size_t stride, uint32_t len, uint16_t d, double* xs, double* ys)
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

        mean = nofft_subrange_mean(s + start * stride, (size_t)(end - start), stride);
        nofft_subrange_pick(s + start * stride, (size_t)(end - start), stride, mean, &pick, &val);

        pos = start + pick;

        xs[i] = (len > 1) ? (2.0 * (double)pos / (double)(len - 1) - 1.0) : 0.0;
        ys[i] = (double)s[pos * stride];
    }
}

static void put_u32le(uint8_t* d, uint32_t v)
{
    d[0] = (uint8_t)(v & 0xFF);
    d[1] = (uint8_t)((v >> 8) & 0xFF);
    d[2] = (uint8_t)((v >> 16) & 0xFF);
    d[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void put_u16le(uint8_t* d, uint16_t v)
{
    d[0] = (uint8_t)(v & 0xFF);
    d[1] = (uint8_t)((v >> 8) & 0xFF);
}

Err wav_write_stream(FILE* out, const pcm_buf* p)
{
    uint8_t hdr[44];
    uint64_t data_bytes;
    uint16_t ch;
    uint32_t block_align;

    if (out == NULL || p->samples == NULL || p->channels == 0 || p->sample_rate == 0)
        return ERR_BAD_ARGS;

    ch = p->channels;
    block_align = (uint32_t)ch * 4u;
    data_bytes = (uint64_t)p->count * 4u;

    if (data_bytes + 36u > 0xFFFFFFFFull)
        return ERR_BAD_ARGS;

    memcpy(hdr + 0, "RIFF", 4);
    put_u32le(hdr + 4, (uint32_t)(data_bytes + 36u));
    memcpy(hdr + 8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    put_u32le(hdr + 16, 16);
    put_u16le(hdr + 20, 3);
    put_u16le(hdr + 22, ch);
    put_u32le(hdr + 24, p->sample_rate);
    put_u32le(hdr + 28, p->sample_rate * block_align);
    put_u16le(hdr + 32, (uint16_t)block_align);
    put_u16le(hdr + 34, 32);
    memcpy(hdr + 36, "data", 4);
    put_u32le(hdr + 40, (uint32_t)data_bytes);

    if (fwrite(hdr, 1, sizeof(hdr), out) != sizeof(hdr))
        return ERR_IO;

    if (p->count > 0 &&
        fwrite(p->samples, 4, p->count, out) != p->count)
        return ERR_IO;

    if (fflush(out) != 0)
        return ERR_IO;

    return ERR_OK;
}

Err nofft_encode(const char* path, const pcm_buf* p, uint32_t frame_len, uint16_t degree,
                 nofft_coeff_format cf)
{
    double f32_sig = 0.0, f32_err = 0.0, f16_sig = 0.0, f16_err = 0.0;
    FILE* f;
    uint64_t total_frames;
    uint32_t n_frames, last_len;
    uint64_t fi;
    uint8_t hdr[NOFFT_HEADER_SIZE];
    uint16_t ch = p->channels;

    if (p->samples == NULL || ch == 0 || frame_len == 0 || degree == 0)
        return ERR_BAD_ARGS;
    if (cf != NOFFT_COEF_F32 && cf != NOFFT_COEF_F16)
        return ERR_BAD_ARGS;

    total_frames = (uint64_t)(p->count / ch);
    if (total_frames == 0)
        return ERR_BAD_ARGS;

    n_frames = (uint32_t)((total_frames + frame_len - 1) / frame_len);
    last_len = (uint32_t)(total_frames - (uint64_t)(n_frames - 1) * frame_len);

    if (nofft_effective_degree(degree, frame_len) > 63 ||
        nofft_effective_degree(degree, last_len) > 63)
        return ERR_BAD_ARGS;

    f = fopen(path, "wb");
    if (f == NULL)
        return ERR_OPEN;

    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr, NOFFT_MAGIC, 4);
    {
        uint32_t v = NOFFT_VERSION;
        memcpy(hdr + 4, &v, 4);
        memcpy(hdr + 8, &p->sample_rate, 4);
        memcpy(hdr + 12, &ch, 2);
        memcpy(hdr + 14, &degree, 2);
        memcpy(hdr + 16, &frame_len, 4);
        memcpy(hdr + 20, &n_frames, 4);
        memcpy(hdr + 24, &last_len, 4);
        {
            uint8_t cfv = (uint8_t)cf;
            memcpy(hdr + 28, &cfv, 1);
        }
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
        double xs[64], ys[64], coefs[64], rounded[64];
        uint16_t ci;

        for (ci = 0; ci < ch; ++ci) {
            Err e;
            int j;

            frame_constraints(p->samples + (fi * ch) + ci, ch, len, d, xs, ys);
            e = nofft_solve_vandermonde(xs, ys, n, coefs);
            if (e != ERR_OK) {
                fclose(f);
                return e;
            }

            if (cf == NOFFT_COEF_F16) {
                uint8_t tmp[128];
                if ((size_t)n * 2u > sizeof(tmp)) {
                    fclose(f);
                    return ERR_RANGE;
                }
                for (j = 0; j < n; ++j) {
                    double c = coefs[j];
                    uint16_t h;
                    if (c > 65504.0 || c < -65504.0) {
                        fclose(f);
                        return ERR_RANGE;
                    }
                    h = f32_to_f16_bits((float)c);
                    tmp[j * 2 + 0] = (uint8_t)(h & 0xFFu);
                    tmp[j * 2 + 1] = (uint8_t)((h >> 8) & 0xFFu);
                }
                for (j = 0; j < n; ++j) {
                    uint16_t h = (uint16_t)((uint16_t)tmp[j * 2] |
                                            ((uint16_t)tmp[j * 2 + 1] << 8));
                    rounded[j] = (double)f16_bits_to_f32(h);
                }
                if (fwrite(tmp, 1, (size_t)n * 2u, f) != (size_t)n * 2u) {
                    fclose(f);
                    return ERR_IO;
                }
            } else {
                uint8_t tmp[256];
                if ((size_t)n * 4u > sizeof(tmp)) {
                    fclose(f);
                    return ERR_RANGE;
                }
                for (j = 0; j < n; ++j) {
                    float v = (float)coefs[j];
                    memcpy(tmp + j * 4, &v, 4);
                    rounded[j] = (double)v;
                }
                if (fwrite(tmp, 1, (size_t)n * 4u, f) != (size_t)n * 4u) {
                    fclose(f);
                    return ERR_IO;
                }
            }

            {
                uint32_t k;
                for (k = 0; k < len; ++k) {
                    double t = (len > 1) ? (2.0 * (double)k / (double)(len - 1) - 1.0) : 0.0;
                    double orig = (double)p->samples[(size_t)(fi + k) * ch + ci];
                    double a = nofft_eval_poly(coefs, n, t) - orig;
                    double b = nofft_eval_poly(rounded, n, t) - orig;
                    f32_sig += orig * orig;
                    f16_sig += orig * orig;
                    f32_err += a * a;
                    f16_err += b * b;
                }
            }
        }
    }

    fclose(f);

    if (cf == NOFFT_COEF_F16 && f16_sig > 0.0) {
        double s32 = (f32_err > 0.0) ? 10.0 * log10(f32_sig / f32_err) : 1.0e9;
        double s16 = (f16_err > 0.0) ? 10.0 * log10(f16_sig / f16_err) : 1.0e9;
        if (s16 < s32 - 6.0)
            fprintf(stderr,
                    "nofft: warning: f16 coefficients reconstruct at %.1f dB "
                    "SNR here (f32 would give %.1f dB); prefer f32 for this "
                    "degree\n",
                    s16, s32);
    }

    return ERR_OK;
}

Err nofft_decode(const char* path, pcm_buf* out)
{
    FILE* f;
    uint8_t hdr[NOFFT_HEADER_SIZE];
    uint32_t version, sr, frame_len, n_frames, last_len;
    uint16_t degree, ch;
    nofft_coeff_format cf = NOFFT_COEF_F32;
    uint64_t total_frames, total_samples;
    uint32_t fi;
    float* buf;
    double* coefs;
    size_t used = 0;

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

    memcpy(&version, hdr + 4, 4);
    memcpy(&sr, hdr + 8, 4);
    memcpy(&ch, hdr + 12, 2);
    memcpy(&degree, hdr + 14, 2);
    memcpy(&frame_len, hdr + 16, 4);
    memcpy(&n_frames, hdr + 20, 4);
    memcpy(&last_len, hdr + 24, 4);

    if (version == NOFFT_VERSION) {
        uint8_t cfv;
        memcpy(&cfv, hdr + 28, 1);
        if (cfv > (uint8_t)NOFFT_COEF_F16) {
            fclose(f);
            return ERR_BAD_NOFFT;
        }
        cf = (nofft_coeff_format)cfv;
    } else if (version != 2) {
        fclose(f);
        return ERR_BAD_NOFFT;
    }

    if (ch == 0 || degree == 0 ||
        frame_len == 0 || n_frames == 0 || last_len == 0 || last_len > frame_len ||
        nofft_effective_degree(degree, frame_len) > 63 ||
        nofft_effective_degree(degree, last_len) > 63) {
        fclose(f);
        return ERR_BAD_NOFFT;
    }

    total_frames = (uint64_t)(n_frames - 1) * frame_len + last_len;
    total_samples = total_frames * ch;

    if (total_samples > (uint64_t)((SIZE_MAX / sizeof(float)) - 1)) {
        fclose(f);
        return ERR_NO_MEMORY;
    }

    buf = (float*)malloc((size_t)total_samples * sizeof(float));
    if (buf == NULL) {
        fclose(f);
        return ERR_NO_MEMORY;
    }

    coefs = (double*)malloc(sizeof(double) * (size_t)(nofft_effective_degree(degree, frame_len) + 1) * ch);
    if (coefs == NULL) {
        free(buf);
        fclose(f);
        return ERR_NO_MEMORY;
    }

    for (fi = 0; fi < n_frames; ++fi) {
        uint32_t len = (fi == n_frames - 1) ? last_len : frame_len;
        uint16_t d = nofft_effective_degree(degree, len);
        int n = (int)d + 1;
        int i;
        uint16_t ci;

        for (ci = 0; ci < ch; ++ci) {
            if (cf == NOFFT_COEF_F16) {
                for (i = 0; i < n; ++i) {
                    uint8_t raw[2];
                    uint16_t h;
                    if (fread(raw, 1, 2, f) != 2) {
                        free(coefs);
                        free(buf);
                        fclose(f);
                        return ERR_BAD_NOFFT;
                    }
                    h = (uint16_t)((uint16_t)raw[0] | ((uint16_t)raw[1] << 8));
                    coefs[(size_t)ci * n + i] = (double)f16_bits_to_f32(h);
                }
            } else {
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
        }

        for (ci = 0; ci < ch; ++ci) {
            uint32_t k;

            for (k = 0; k < len; ++k) {
                double t = (len > 1) ? (2.0 * (double)k / (double)(len - 1) - 1.0) : 0.0;
                buf[used + (size_t)k * ch + ci] =
                    (float)nofft_eval_poly(coefs + (size_t)ci * n, n, t);
            }
        }

        used += (size_t)len * ch;
    }

    free(coefs);
    fclose(f);

    out->samples = buf;
    out->count = used;
    out->sample_rate = sr;
    out->channels = ch;
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