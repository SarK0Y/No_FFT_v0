#include "acpcm.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

/* ---- LZMA style carryless range coder -------------------------------
 *
 * `low` must be 64 bit.  A carry out of the bound addition lives above bit 31
 * and is consumed by the byte output; truncating low to 32 bits loses it and
 * the stream decodes to garbage.  Adaptation is a 5 bit shift of the distance
 * to 2048, one shared model per call for fixed width fields.
 */
namespace {

const uint16_t RC_PROB_INIT = 2048 >> 1;
const uint32_t RC_TOP       = 1u << 24;

void rc_shift_low(std::vector<uint8_t>& out, uint64_t& low,
                  uint32_t& range, uint8_t& cache, int64_t& cache_size)
{
    if ((uint32_t)(low >> 32) != 0 || (uint32_t)low < 0xFF000000u) {
        uint8_t carry = (uint8_t)(low >> 32);
        uint8_t temp = cache;
        do {
            out.push_back((uint8_t)(temp + carry));
            temp = 0xFF;
        } while (--cache_size != 0);
        cache = (uint8_t)((uint32_t)low >> 24);
    }
    cache_size++;
    low = (uint32_t)low << 8;
    range <<= 8;
}

void rc_enc_bit(std::vector<uint8_t>& out, uint64_t& low, uint32_t& range,
                uint8_t& cache, int64_t& cache_size, uint16_t* prob, int bit)
{
    uint32_t bound = (range >> 11) * (uint32_t)(*prob);
    if (bit) {
        low += bound;
        range -= bound;
        *prob = (uint16_t)(*prob - (*prob >> 5));
    } else {
        range = bound;
        *prob = (uint16_t)(*prob + ((2048u - (uint32_t)(*prob)) >> 5));
    }
    while (range < RC_TOP)
        rc_shift_low(out, low, range, cache, cache_size);
}

/* Five shift-lows, not a run of dummy bits: a dummy-bit flush can leave the
   final interval undecodable, which shows up as the last couple of samples of
   every file coming out wrong. */
void rc_flush(std::vector<uint8_t>& out, uint64_t& low, uint32_t& range,
              uint8_t& cache, int64_t& cache_size)
{
    for (int i = 0; i < 5; i++)
        rc_shift_low(out, low, range, cache, cache_size);
}

void rc_enc_fixed(std::vector<uint8_t>& out, uint64_t& low, uint32_t& range,
                  uint8_t& cache, int64_t& cache_size, uint32_t v, int n)
{
    uint16_t p = RC_PROB_INIT;
    for (int i = n - 1; i >= 0; i--)
        rc_enc_bit(out, low, range, cache, cache_size, &p, (int)((v >> i) & 1u));
}

struct EncCtx {
    std::vector<uint8_t> out;
    uint64_t low;
    uint32_t range;
    uint8_t cache;
    int64_t cache_size;

    EncCtx() : low(0), range(0xFFFFFFFFu), cache(0), cache_size(1) {}

    void bit(uint16_t* prob, int b)
    {
        rc_enc_bit(out, low, range, cache, cache_size, prob, b);
    }
    void fixed(uint32_t v, int n)
    {
        rc_enc_fixed(out, low, range, cache, cache_size, v, n);
    }
    void done() { rc_flush(out, low, range, cache, cache_size); }
};

/* ---- decoder ---- */

struct DecCtx {
    const uint8_t* d;
    size_t n;
    size_t pos;
    uint32_t range;
    uint32_t code;

    void norm()
    {
        while (range < RC_TOP) {
            range <<= 8;
            code = (code << 8) | (pos < n ? (uint32_t)d[pos++] : 0u);
        }
    }

    int bit(uint16_t* prob)
    {
        uint32_t bound = (range >> 11) * (uint32_t)(*prob);
        int b;
        if (code < bound) {
            b = 0;
            range = bound;
            *prob = (uint16_t)(*prob + ((2048u - (uint32_t)(*prob)) >> 5));
        } else {
            b = 1;
            code -= bound;
            range -= bound;
            *prob = (uint16_t)(*prob - (*prob >> 5));
        }
        norm();
        return b;
    }

    uint32_t fixed(int n)
    {
        uint32_t v = 0;
        uint16_t p = RC_PROB_INIT;
        for (int i = 0; i < n; i++)
            v = (v << 1) | (uint32_t)bit(&p);
        return v;
    }
};

/* ---- symbol model ----
 *
 * zero flag, then a unary "how many bits is the magnitude" run, then that many
 * mantissa bits, then the sign.  A q of 0 is by far the most common value so it
 * gets the single adaptive flag.
 */
struct SymModel {
    uint16_t p_zero;
    uint16_t p_sign;
    uint16_t p_run[64];
};

void sym_init(SymModel* m)
{
    m->p_zero = RC_PROB_INIT;
    m->p_sign = RC_PROB_INIT;
    for (int i = 0; i < 64; i++)
        m->p_run[i] = RC_PROB_INIT;
}

void enc_sym(EncCtx* e, SymModel* m, int32_t q)
{
    if (q == 0) {
        e->bit(&m->p_zero, 0);
        return;
    }
    e->bit(&m->p_zero, 1);
    uint32_t mag = (q < 0) ? (uint32_t)(-q) : (uint32_t)q;
    int k = 0;
    while ((mag >> k) != 0)
        k++;
    for (int j = 1; j < k; j++)
        e->bit(&m->p_run[j], 0);
    e->bit(&m->p_run[k], 1);
    for (int j = k - 2; j >= 0; j--)
        e->fixed((mag >> j) & 1u, 1);
    e->bit(&m->p_sign, q < 0 ? 1 : 0);
}

int32_t dec_sym(DecCtx* d, SymModel* m)
{
    if (d->bit(&m->p_zero) == 0)
        return 0;
    int k = 1;
    while (d->bit(&m->p_run[k]) == 0)
        k++;
    uint32_t mag = 1;
    for (int j = k - 2; j >= 0; j--)
        mag = (mag << 1) | d->fixed(1);
    int32_t q = (int32_t)mag;
    return d->bit(&m->p_sign) ? -q : q;
}

/* ---- little endian helpers ----
 *
 * The polynomial format in nofft.cpp serialises with memcpy of native ints, so
 * its files are host endian.  These are explicit instead.
 */
void put_u32(uint8_t* d, uint32_t v)
{
    d[0] = (uint8_t)(v & 0xFF);
    d[1] = (uint8_t)((v >> 8) & 0xFF);
    d[2] = (uint8_t)((v >> 16) & 0xFF);
    d[3] = (uint8_t)((v >> 24) & 0xFF);
}

uint32_t get_u32(const uint8_t* d)
{
    return (uint32_t)d[0] | ((uint32_t)d[1] << 8) |
           ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
}

/* Fixed point rounding, always half away from zero, matching llround. */
double quant_fixed(double v, double scale);

/* Scales, rounds, then clamps into the range the wire field can hold.  Clamping
   matters: a normalised sample at the channel peak rounds to exactly 32768,
   which wraps to -32768 as a signed 16 bit field and flips the sign of the
   first reconstructed sample. */
int32_t quant_clamp(double v, double scale, double lo, double hi)
{
    double s = quant_fixed(v, scale);
    if (s < lo)
        s = lo;
    if (s > hi)
        s = hi;
    return (int32_t)s;
}

double quant_fixed(double v, double scale)
{
    double s = v * scale;
    double r = (s >= 0.0) ? std::floor(s + 0.5) : std::ceil(s - 0.5);
    return r;
}

/* Predictor gain is held strictly inside the unit circle.  An open loop fit of
   this material wants a1 = 1.437, whose pole sits at 1.72 and whose closed
   loop saturates; clamping costs nothing here because the real fit is 0.969. */
const double A1_LIMIT = 0.999;

struct ChanPlan {
    uint16_t peak_q;
    int16_t mean_q;
    int16_t a1_q;
    uint16_t step_q;
    double peak;
    double mean;
    double a1;
    double step;
};

/* Header fields are 16 bit fixed point over a nominal [-1, 1] signal.  Input
   outside that range cannot be represented, and clamping it into range silently
   produces a stream that decodes to noise, so reject it instead. */
const double PEAK_LIMIT = 2.0;

/* One pass over a channel: peak, mean, least squares gain, residual peak. */
Err plan_channel(const float* s, size_t n, uint16_t bits, ChanPlan* pl)
{
    double peak = 0.0, mean = 0.0;
    for (size_t i = 0; i < n; i++) {
        double v = s[i];
        double a = (v < 0.0) ? -v : v;
        if (!(a == a) || a > PEAK_LIMIT)
            return ERR_BAD_INPUT; /* NaN, or too hot for the header fields */
        if (a > peak)
            peak = a;
        mean += v;
    }
    mean /= (double)n;
    if (peak < 1e-9)
        peak = 1.0; /* silence: avoid dividing by zero */

    double num = 0.0, den = 0.0;
    for (size_t i = 1; i < n; i++) {
        double p = (s[i - 1] - mean) / peak;
        double c = (s[i] - mean) / peak;
        num += p * c;
        den += p * p;
    }

    double a1 = (den > 1e-12) ? (num / den) : 0.0;
    if (a1 > A1_LIMIT)
        a1 = A1_LIMIT;
    if (a1 < -A1_LIMIT)
        a1 = -A1_LIMIT;

    /* residual peak sets the step so the largest residual lands just inside
       the quantiser range */
    double rpk = 0.0;
    for (size_t i = 1; i < n; i++) {
        double p = (s[i - 1] - mean) / peak;
        double c = (s[i] - mean) / peak;
        double r = c - a1 * p;
        double ar = (r < 0.0) ? -r : r;
        if (ar > rpk)
            rpk = ar;
    }

    double step = 2.0 * rpk / (double)((1u << bits) - 1u);
    if (step < 1e-9)
        step = 1.0 / 32768.0;

    /* Quantise once here, then write the quantised value.  Doing the rounding
       again on the decode side shifts the gain by one LSB and the recursive
       filter never resynchronises. */
    pl->peak_q = (uint16_t)quant_clamp(peak, 32768.0, 0.0, 65535.0);
    pl->mean_q = (int16_t)quant_clamp(mean, 32768.0, -32768.0, 32767.0);
    pl->a1_q = (int16_t)quant_clamp(a1, 16384.0, -32768.0, 32767.0);
    pl->step_q = (uint16_t)quant_clamp(step, 32768.0, 1.0, 65535.0);

    pl->peak = (double)pl->peak_q / 32768.0;
    pl->mean = (double)pl->mean_q / 32768.0;
    pl->a1 = (double)pl->a1_q / 16384.0;
    pl->step = (double)pl->step_q / 32768.0;
    if (pl->peak < 1e-9)
        pl->peak = 1.0;

    return ERR_OK;
}

Err encode_chunk(const float* s, size_t n, const ChanPlan& pl,
                 uint16_t bits, std::vector<uint8_t>* out)
{
    int32_t lim = (int32_t)((1u << (bits - 1)) - 1u);
    SymModel m;
    sym_init(&m);

    EncCtx e;
    e.fixed(pl.peak_q, 16);
    e.fixed((uint32_t)(uint16_t)pl.mean_q, 16);
    e.fixed((uint32_t)(uint16_t)pl.a1_q, 16);
    e.fixed(pl.step_q, 16);
    e.fixed(bits, 8);

    double x0 = (quant_fixed(s[0], 32768.0) / 32768.0 - pl.mean) / pl.peak;
    double xh = x0;
    e.fixed((uint32_t)(uint16_t)quant_clamp(x0, 32768.0, -32768.0, 32767.0), 16);

    for (size_t i = 1; i < n; i++) {
        double src = (s[i] - pl.mean) / pl.peak;
        double pred = pl.a1 * xh;
        int32_t q = (int32_t)quant_fixed((src - pred) / pl.step, 1.0);
        if (q > lim - 1)
            q = lim - 1;
        if (q < -lim)
            q = -lim;
        enc_sym(&e, &m, q);
        xh = pred + (double)q * pl.step;
    }

    e.done();
    out->swap(e.out);
    return ERR_OK;
}

Err decode_chunk(const uint8_t* d, size_t n, size_t* consumed, uint16_t bits,
                 std::vector<float>* out)
{
    DecCtx c;
    c.d = d;
    c.n = n;
    c.pos = 0;
    c.range = 0xFFFFFFFFu;
    c.code = 0;
    for (int i = 0; i < 5; i++)
        c.code = (c.code << 8) | (uint32_t)(c.pos < c.n ? d[c.pos++] : 0u);

    uint32_t peak_q = c.fixed(16);
    int32_t mean_q = (int32_t)(int16_t)(uint16_t)c.fixed(16);
    int32_t a1_q = (int32_t)(int16_t)(uint16_t)c.fixed(16);
    uint32_t step_q = c.fixed(16);
    uint32_t hb = c.fixed(8);

    if (hb != (uint32_t)bits)
        return ERR_BAD_NOFFT;
    if (peak_q == 0 || step_q == 0)
        return ERR_BAD_NOFFT;

    double peak = (double)peak_q / 32768.0;
    double mean = (double)mean_q / 32768.0;
    double a1 = (double)a1_q / 16384.0;
    double step = (double)step_q / 32768.0;

    int32_t lim = (int32_t)((1u << (bits - 1)) - 1u);
    SymModel m;
    sym_init(&m);

    size_t frames = out->size();
    if (frames == 0)
        return ERR_BAD_NOFFT;

    double x0 = (double)(int32_t)(int16_t)(uint16_t)c.fixed(16) / 32768.0;
    double xh = x0;
    (*out)[0] = (float)(x0 * peak + mean);

    for (size_t i = 1; i < frames; i++) {
        int32_t q = dec_sym(&c, &m);
        if (q > lim - 1)
            q = lim - 1;
        if (q < -lim)
            q = -lim;
        double pred = a1 * xh;
        xh = pred + (double)q * step;
        (*out)[i] = (float)(xh * peak + mean);
    }

    if (consumed != NULL)
        *consumed = c.pos;
    return ERR_OK;
}

} /* namespace */

Err acpcm_encode(const char* path, const pcm_buf* p, uint16_t bits)
{
    if (p == NULL || p->samples == NULL || p->channels == 0)
        return ERR_BAD_ARGS;
    if (bits < ACPCM_BITS_MIN || bits > ACPCM_BITS_MAX)
        return ERR_RANGE;

    size_t ch = p->channels;
    size_t frames = p->count / ch;
    if (frames == 0)
        return ERR_BAD_ARGS;
    if (frames > 0xFFFFFFFFull)
        return ERR_RANGE;

    /* Gather each channel so the encoder and the analyser agree on the order
       samples are visited in; a stride parameter here is an easy way to get a
       subtly different plan from the one that was measured. */
    std::vector<std::vector<uint8_t> > chunks(ch);
    for (size_t c = 0; c < ch; c++) {
        std::vector<float> gath(frames);
        for (size_t i = 0; i < frames; i++)
            gath[i] = p->samples[i * ch + c];
        ChanPlan pl;
        Err e = plan_channel(gath.data(), frames, bits, &pl);
        if (e != ERR_OK)
            return e;
        e = encode_chunk(gath.data(), frames, pl, bits, &chunks[c]);
        if (e != ERR_OK)
            return e;
    }

    uint64_t payload = 0;
    for (size_t c = 0; c < ch; c++)
        payload += chunks[c].size();

    size_t hsz = ACPCM_FIXED_HEADER + 4 * ch;
    std::vector<uint8_t> hdr(hsz, 0);
    memcpy(hdr.data(), ACPCM_MAGIC, 4);
    put_u32(hdr.data() + 4, (uint32_t)frames);
    put_u32(hdr.data() + 8, (uint32_t)bits);
    put_u32(hdr.data() + 12, (uint32_t)ch);
    put_u32(hdr.data() + 16, p->sample_rate);
    for (size_t c = 0; c < ch; c++)
        put_u32(hdr.data() + ACPCM_FIXED_HEADER + 4 * c, (uint32_t)chunks[c].size());

    FILE* f = fopen(path, "wb");
    if (f == NULL)
        return ERR_OPEN;
    Err rc = ERR_OK;
    if (fwrite(hdr.data(), 1, hdr.size(), f) != hdr.size())
        rc = ERR_IO;
    for (size_t c = 0; c < ch && rc == ERR_OK; c++)
        if (fwrite(chunks[c].data(), 1, chunks[c].size(), f) != chunks[c].size())
            rc = ERR_IO;
    if (fclose(f) != 0 && rc == ERR_OK)
        rc = ERR_IO;
    return rc;
}

Err acpcm_file_info(const char* path, acpcm_info* out)
{
    if (out == NULL)
        return ERR_BAD_ARGS;

    FILE* f = fopen(path, "rb");
    if (f == NULL)
        return ERR_OPEN;

    uint8_t hdr[ACPCM_FIXED_HEADER];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
        memcmp(hdr, ACPCM_MAGIC, 4) != 0) {
        fclose(f);
        return ERR_BAD_NOFFT;
    }

    uint32_t frames = get_u32(hdr + 4);
    uint32_t bits = get_u32(hdr + 8);
    uint32_t ch = get_u32(hdr + 12);
    uint32_t rate = get_u32(hdr + 16);
    if (ch == 0 || ch > 64 || rate == 0 ||
        bits < ACPCM_BITS_MIN || bits > ACPCM_BITS_MAX) {
        fclose(f);
        return ERR_BAD_NOFFT;
    }

    uint8_t lenbuf[256];
    if (ch > 64 || fread(lenbuf, 1, 4 * ch, f) != 4 * ch) {
        fclose(f);
        return ERR_BAD_NOFFT;
    }

    uint64_t payload = 0;
    for (uint32_t c = 0; c < ch; c++)
        payload += get_u32(lenbuf + 4 * c);

    out->bits = (uint16_t)bits;
    out->channels = (uint16_t)ch;
    out->sample_rate = rate;
    out->frames = frames;
    out->payload = payload;
    out->total = ACPCM_FIXED_HEADER + (uint64_t)4 * ch + payload;

    fclose(f);
    return ERR_OK;
}

Err acpcm_decode(const char* path, pcm_buf* out)
{
    memset(out, 0, sizeof(*out));

    acpcm_info info;
    Err ie = acpcm_file_info(path, &info);
    if (ie != ERR_OK)
        return ie;

    size_t ch = info.channels;
    size_t frames = (size_t)info.frames;
    uint16_t bits = info.bits;

    if (frames == 0)
        return ERR_BAD_NOFFT;
    if (frames > (SIZE_MAX / sizeof(float)) / (ch == 0 ? 1 : ch))
        return ERR_NO_MEMORY;

    FILE* f = fopen(path, "rb");
    if (f == NULL)
        return ERR_OPEN;

    std::vector<uint8_t> lenbuf(4 * ch);
    if (fseek(f, (long)(ACPCM_FIXED_HEADER), SEEK_SET) != 0 ||
        fread(lenbuf.data(), 1, lenbuf.size(), f) != lenbuf.size()) {
        fclose(f);
        return ERR_IO;
    }

    std::vector<std::vector<uint8_t> > raw(ch);
    for (size_t c = 0; c < ch; c++) {
        uint32_t len = get_u32(lenbuf.data() + 4 * c);
        if (len == 0) {
            fclose(f);
            return ERR_BAD_NOFFT;
        }
        raw[c].resize(len);
        if (fread(raw[c].data(), 1, len, f) != len) {
            fclose(f);
            return ERR_IO;
        }
    }
    fclose(f);

    float* buf = (float*)malloc(frames * ch * sizeof(float));
    if (buf == NULL)
        return ERR_NO_MEMORY;

    std::vector<float> scratch(frames);
    for (size_t c = 0; c < ch; c++) {
        size_t used = 0;
        scratch.assign(frames, 0.0f);
        Err e = decode_chunk(raw[c].data(), raw[c].size(), &used, bits, &scratch);
        if (e != ERR_OK) {
            free(buf);
            return e;
        }
        for (size_t i = 0; i < frames; i++)
            buf[i * ch + c] = scratch[i];
    }

    out->samples = buf;
    out->count = frames * ch;
    out->channels = (uint16_t)ch;
    out->sample_rate = info.sample_rate;

    return ERR_OK;
}