#include "ext.h"
#include <arm_neon.h>

/* Time Lock's hit detector: SuperFlux onset strength, peak picking, then the
 * zero crossing before each attack. Runs in the foreground, one step a call. */

#define FFT_POINTS        2048u
#define FFT_HALF          (FFT_POINTS / 2u)    /* the complex FFT that does the real one */
#define FFT_QUARTER       (FFT_HALF / 4u)
#define FFT_BINS          (FFT_HALF + 1u)
#define BANDS             80u
#define BAND_LOW_HZ       30.0
#define BAND_HIGH_HZ      16000.0
#define BAND_WEIGHTS_MAX  2560u
#define LOG_GAMMA         20.0f
#define HOP_MS            10u
#define STRENGTH_MAX      3072u                /* frames: about 30 s analysed */
#define ENVELOPE_MAX      (STRENGTH_MAX * HOP_MS + HOP_MS) /* its 1 ms blocks */
#define CANDIDATES_MAX    2048u
#define BLOCK_HZ          1000u                /* the attack envelope's 1 ms blocks */
#define ATTACK_BLOCKS     25                   /* searched each side of the peak's frame */
#define ATTACK_FRACTION   0.2
#define ZERO_BLOCKS       4u                   /* how far back the zero crossing is looked for */
#define Q31_SCALE         (1.0f / 2147483648.0f)
/* Peak picking for hits and rises. */
#define HIT_DELTA         0.03
#define HIT_MAX_MS        15u
#define RISE_DELTA        0.01
#define RISE_MAX_MS       10u
#define PEAK_AVG_MS       50u
#define PEAK_PERCENTILE   0.95

/* IEEE single fields for the vector logarithm. */
#define FLOAT_EXPONENT_SHIFT 23
#define FLOAT_EXPONENT_HALF  126                /* biased exponent of [0.5, 1) */
#define FLOAT_MANTISSA_MASK  0x807fffffu        /* sign and mantissa */
#define FLOAT_HALF_EXPONENT  0x3f000000u        /* exponent field of 0.5 */

enum { STAGE_IDLE, STAGE_STRENGTH, STAGE_PICK, STAGE_ATTACK, STAGE_DONE };

static struct {
    const s32 *left, *right;
    u32 frames, rate, hop, block, blocks;
    u32 stage, next, strength_frames, candidates, results, envelope_blocks;
    u32 table_rate, bins_used; /* the bins the bands read */
    float scale;
} job;

#define ALIGNED __attribute__((aligned(16)))
/* The FFT's two buffers, real and imaginary parts apart; the result's
 * extra element repeats element 0 for the split. */
static float fft_re[2][FFT_HALF + 4] ALIGNED, fft_im[2][FFT_HALF + 4] ALIGNED;
static float window[FFT_POINTS] ALIGNED;              /* Hann, over full scale: Q31 to +-1 */
static float first_twiddle[6][FFT_QUARTER] ALIGNED;   /* w^p, w^2p, w^3p of the first pass: re, im each */
static float twiddle[FFT_HALF][2] ALIGNED;            /* e^(-2 pi i k / FFT_HALF): re, im */
static float split_re[FFT_HALF / 2] ALIGNED, split_im[FFT_HALF / 2] ALIGNED; /* e^(-2 pi i k / FFT_POINTS) */
static float magnitude[FFT_BINS + 7] ALIGNED;
static float band_weights[BAND_WEIGHTS_MAX] ALIGNED;
static u16 band_first[BANDS], band_count[BANDS], band_offset[BANDS];
static float bands[2][BANDS + 4] ALIGNED;             /* [1..BANDS]: edge copies at 0 and BANDS + 1 */
static float strength[STRENGTH_MAX];
static union {
    float order[STRENGTH_MAX];                        /* the percentile's copy of the strength */
    s32 edge[FFT_POINTS];                             /* a frame that runs past the sample: mono, zero-padded */
} scratch ALIGNED;
static u32 candidate[CANDIDATES_MAX];
static u32 result[CANDIDATES_MAX];
static float envelope[ENVELOPE_MAX];                  /* RMS of each 1 ms block, found with the strength */
static const float *spectrum_re, *spectrum_im;        /* the buffer the last pass wrote */

/* ---- double-precision helpers for the tables (no libm) ---- */

static double ln(double x)
{
    static const double ln2 = 0.69314718055994530942;
    int e = 0;
    while (x >= 2.0) { x *= 0.5; ++e; }
    while (x < 1.0) { x *= 2.0; --e; }
    double z = (x - 1.0) / (x + 1.0), z2 = z * z, term = z, sum = 0.0;
    for (u32 n = 1; n < 60; n += 2) { sum += term / n; term *= z2; }
    return 2.0 * sum + e * ln2;
}

static double expo(double y)
{
    static const double ln2 = 0.69314718055994530942;
    int e = 0;
    while (y > ln2) { y -= ln2; ++e; }
    while (y < 0.0) { y += ln2; --e; }
    double term = 1.0, sum = 1.0;
    for (u32 n = 1; n < 30; ++n) { term *= y / n; sum += term; }
    while (e > 0) { sum *= 2.0; --e; }
    while (e < 0) { sum *= 0.5; ++e; }
    return sum;
}

/* cos and sin of a small angle by their series. */
static void small_turn(double a, double *c, double *s)
{
    double a2 = a * a;
    *c = 1.0 - a2 / 2 * (1.0 - a2 / 12 * (1.0 - a2 / 30 * (1.0 - a2 / 56)));
    *s = a * (1.0 - a2 / 6 * (1.0 - a2 / 20 * (1.0 - a2 / 42 * (1.0 - a2 / 72))));
}

/* Round to nearest, ties to even, as Python's round(). */
static u32 round_even(double x)
{
    u32 n = (u32)x;
    double f = x - n;
    return f > 0.5 || (f == 0.5 && (n & 1u)) ? n + 1u : n;
}

static void fft_tables(void)
{
    static const double pi = 3.14159265358979323846;
    static double turn[FFT_POINTS][2]; /* e^(-2 pi i k / FFT_POINTS) */
    double c, s, wr = 1.0, wi = 0.0;
    small_turn(2.0 * pi / FFT_POINTS, &c, &s);
    for (u32 k = 0; k < FFT_POINTS; ++k) {
        turn[k][0] = wr; turn[k][1] = -wi;
        double r = wr * c - wi * s; wi = wr * s + wi * c; wr = r;
    }
    for (u32 k = 0; k < FFT_HALF / 2; ++k) { split_re[k] = (float)turn[k][0]; split_im[k] = (float)turn[k][1]; }
    for (u32 k = 0; k < FFT_HALF; ++k) { twiddle[k][0] = (float)turn[2 * k][0]; twiddle[k][1] = (float)turn[2 * k][1]; }
    for (u32 p = 0; p < FFT_QUARTER; ++p)
        for (u32 j = 0; j < 3; ++j) {
            first_twiddle[2 * j][p] = twiddle[(j + 1) * p][0];
            first_twiddle[2 * j + 1][p] = twiddle[(j + 1) * p][1];
        }
    /* numpy's hanning: symmetric, period FFT_POINTS - 1 */
    small_turn(2.0 * pi / (FFT_POINTS - 1), &c, &s);
    wr = 1.0; wi = 0.0;
    for (u32 n = 0; n < FFT_POINTS; ++n) {
        window[n] = (float)(0.5 - 0.5 * wr) * Q31_SCALE;
        double r = wr * c - wi * s; wi = wr * s + wi * c; wr = r;
    }
}

/* Triangular bands between geometrically spaced edges; each band's weights
 * padded to whole vectors. */
static void band_tables(u32 rate)
{
    double high = rate / 2.0 < BAND_HIGH_HZ ? rate / 2.0 : BAND_HIGH_HZ;
    double edges[BANDS + 2], step = (ln(high) - ln(BAND_LOW_HZ)) / (BANDS + 1);
    for (u32 i = 0; i < BANDS + 2; ++i) edges[i] = expo(ln(BAND_LOW_HZ) + step * i);
    edges[0] = BAND_LOW_HZ; edges[BANDS + 1] = high;
    double bin_hz = (double)rate / FFT_POINTS;
    u32 offset = 0;
    job.bins_used = 0;
    for (u32 b = 0; b < BANDS; ++b) {
        double lo = edges[b], mid = edges[b + 1], hi = edges[b + 2];
        u32 first = FFT_BINS, last = 0;
        for (u32 k = 0; k < FFT_BINS; ++k) {
            double f = k * bin_hz;
            if (f > lo && f < hi) { if (first == FFT_BINS) first = k; last = k; }
        }
        band_offset[b] = (u16)offset;
        if (first == FFT_BINS) { band_first[b] = 0; band_count[b] = 0; continue; }
        u32 count = (last - first + 4u) & ~3u;
        band_first[b] = (u16)first; band_count[b] = (u16)count;
        for (u32 i = 0; i < count; ++i) {
            double f = (first + i) * bin_hz;
            double up = (f - lo) / (mid - lo), down = (hi - f) / (hi - mid);
            double w = up < down ? up : down;
            band_weights[offset + i] = 0.5f * (float)(w < 0.0 ? 0.0 : w > 1.0 ? 1.0 : w); /* magnitudes are 2|X| */
        }
        offset += count;
        if (first + count > job.bins_used) job.bins_used = first + count;
    }
    job.table_rate = rate;
}

/* ---- one strength frame ---- */

/* Four Q31 frames as floats. */
static inline float32x4_t frames4(const s32 *p) { return vcvtq_f32_s32(vld1q_s32((const int32_t *)p)); }

static inline float32x4_t reverse4(float32x4_t v)
{
    float32x4_t r = vrev64q_f32(v);
    return vcombine_f32(vget_high_f32(r), vget_low_f32(r));
}

/* One Newton step on the estimate: enough for every bench loop's hits
 * (the estimate alone moves a rise). */
static inline float32x4_t square_root(float32x4_t x)
{
    x = vmaxq_f32(x, vdupq_n_f32(1e-30f));
    float32x4_t e = vrsqrteq_f32(x);
    e = vmulq_f32(e, vrsqrtsq_f32(vmulq_f32(x, e), e));
    return vmulq_f32(x, e);
}

/* Natural log of x >= 1 (cephes logf). */
static inline float32x4_t logarithm(float32x4_t x)
{
    uint32x4_t bits = vreinterpretq_u32_f32(x);
    int32x4_t e = vsubq_s32(vreinterpretq_s32_u32(vshrq_n_u32(bits, FLOAT_EXPONENT_SHIFT)), vdupq_n_s32(FLOAT_EXPONENT_HALF));
    float32x4_t mant = vreinterpretq_f32_u32(vorrq_u32(vandq_u32(bits, vdupq_n_u32(FLOAT_MANTISSA_MASK)), vdupq_n_u32(FLOAT_HALF_EXPONENT)));
    uint32x4_t low = vcltq_f32(mant, vdupq_n_f32(0.707106781186547524f));
    e = vsubq_s32(e, vreinterpretq_s32_u32(vandq_u32(low, vdupq_n_u32(1))));
    mant = vaddq_f32(mant, vreinterpretq_f32_u32(vandq_u32(low, vreinterpretq_u32_f32(mant))));
    float32x4_t v = vsubq_f32(mant, vdupq_n_f32(1.0f)), z = vmulq_f32(v, v), fe = vcvtq_f32_s32(e);
    float32x4_t p = vdupq_n_f32(7.0376836292e-2f);
    p = vmlaq_f32(vdupq_n_f32(-1.1514610310e-1f), p, v);
    p = vmlaq_f32(vdupq_n_f32(1.1676998740e-1f), p, v);
    p = vmlaq_f32(vdupq_n_f32(-1.2420140846e-1f), p, v);
    p = vmlaq_f32(vdupq_n_f32(1.4249322787e-1f), p, v);
    p = vmlaq_f32(vdupq_n_f32(-1.6668057665e-1f), p, v);
    p = vmlaq_f32(vdupq_n_f32(2.0000714765e-1f), p, v);
    p = vmlaq_f32(vdupq_n_f32(-2.4999993993e-1f), p, v);
    p = vmlaq_f32(vdupq_n_f32(3.3333331174e-1f), p, v);
    float32x4_t y = vmulq_f32(vmulq_f32(p, v), z);
    y = vmlaq_f32(y, fe, vdupq_n_f32(-2.12194440e-4f));
    y = vmlsq_f32(y, z, vdupq_n_f32(0.5f));
    v = vaddq_f32(v, y);
    return vmlaq_f32(v, fe, vdupq_n_f32(0.693359375f));
}

/* z[p + k * FFT_QUARTER] for four p, from the frame: x[2n] the real part,
 * x[2n + 1] the imaginary part, windowed and scaled. */
static inline void frame_points(const s32 *l, const s32 *r, u32 n, float32x4_t *zr, float32x4_t *zi)
{
    int32x4x2_t a = vld2q_s32((const int32_t *)l + n);
    float32x4x2_t w = vld2q_f32(window + n);
    if (r) { /* the mean of the sides */
        int32x4x2_t b = vld2q_s32((const int32_t *)r + n);
        a.val[0] = vhaddq_s32(a.val[0], b.val[0]); a.val[1] = vhaddq_s32(a.val[1], b.val[1]);
    }
    *zr = vmulq_f32(vcvtq_f32_s32(a.val[0]), w.val[0]); *zi = vmulq_f32(vcvtq_f32_s32(a.val[1]), w.val[1]);
}

/* The radix-4 butterfly of a Stockham pass: outputs before their twiddles. */
#define BUTTERFLY(ar, ai, br, bi, cr, ci, dr, di)                                                        \
    float32x4_t pcr = vaddq_f32(ar, cr), pci = vaddq_f32(ai, ci), mcr = vsubq_f32(ar, cr), mci = vsubq_f32(ai, ci); \
    float32x4_t pdr = vaddq_f32(br, dr), pdi = vaddq_f32(bi, di), mdr = vsubq_f32(br, dr), mdi = vsubq_f32(bi, di); \
    float32x4_t y0r = vaddq_f32(pcr, pdr), y0i = vaddq_f32(pci, pdi);                                    \
    float32x4_t y2r = vsubq_f32(pcr, pdr), y2i = vsubq_f32(pci, pdi);                                    \
    float32x4_t y1r = vaddq_f32(mcr, mdi), y1i = vsubq_f32(mci, mdr); /* (a - c) - i (b - d) */          \
    float32x4_t y3r = vsubq_f32(mcr, mdi), y3i = vaddq_f32(mci, mdr)  /* (a - c) + i (b - d) */

static inline void turn_vector(float32x4_t *xr, float32x4_t *xi, float32x4_t wr, float32x4_t wi)
{
    float32x4_t r = vmlsq_f32(vmulq_f32(*xr, wr), *xi, wi);
    *xi = vmlaq_f32(vmulq_f32(*xr, wi), *xi, wr);
    *xr = r;
}

static inline void turn_lane(float32x4_t *xr, float32x4_t *xi, float32x2_t w)
{
    float32x4_t r = vmlsq_lane_f32(vmulq_lane_f32(*xr, w, 0), *xi, w, 1);
    *xi = vmlaq_lane_f32(vmulq_lane_f32(*xr, w, 1), *xi, w, 0);
    *xr = r;
}

/* The first pass (length FFT_HALF, stride 1), four p at a time; its
 * outputs y[4p + j] interleave. */
static void first_pass(u32 t)
{
    s32 base = (s32)(t * job.hop) - (s32)FFT_HALF;
    const s32 *l, *r = 0;
    if (base >= 0 && (u32)base + FFT_POINTS <= job.frames) {
        l = job.left + base;
        if (job.right) r = job.right + base;
    } else {
        for (u32 n = 0; n < FFT_POINTS; ++n) {
            s32 at = base + (s32)n;
            s32 x = 0;
            if (at >= 0 && (u32)at < job.frames) x = job.right ? (s32)(((s64)job.left[at] + job.right[at]) >> 1) : job.left[at];
            scratch.edge[n] = x;
        }
        l = scratch.edge;
    }
    /* the next frame's new samples */
    for (u32 n = 0; n < job.hop; n += CACHE_LINE_BYTES / sizeof(s32)) {
        __builtin_prefetch(l + FFT_POINTS + n);
        if (r) __builtin_prefetch(r + FFT_POINTS + n);
    }
    float *yr = fft_re[0], *yi = fft_im[0];
    for (u32 p = 0; p < FFT_QUARTER; p += 4) {
        float32x4_t ar, ai, br, bi, cr, ci, dr, di;
        frame_points(l, r, 2 * p, &ar, &ai);
        frame_points(l, r, 2 * (p + FFT_QUARTER), &br, &bi);
        frame_points(l, r, 2 * (p + 2 * FFT_QUARTER), &cr, &ci);
        frame_points(l, r, 2 * (p + 3 * FFT_QUARTER), &dr, &di);
        BUTTERFLY(ar, ai, br, bi, cr, ci, dr, di);
        turn_vector(&y1r, &y1i, vld1q_f32(first_twiddle[0] + p), vld1q_f32(first_twiddle[1] + p));
        turn_vector(&y2r, &y2i, vld1q_f32(first_twiddle[2] + p), vld1q_f32(first_twiddle[3] + p));
        turn_vector(&y3r, &y3i, vld1q_f32(first_twiddle[4] + p), vld1q_f32(first_twiddle[5] + p));
        float32x4x4_t outr = {{y0r, y1r, y2r, y3r}}, outi = {{y0i, y1i, y2i, y3i}};
        vst4q_f32(yr + 4 * p, outr); vst4q_f32(yi + 4 * p, outi);
    }
}

/* The other passes: length n, stride s = FFT_HALF / n, vectors along q. */
static void fft_rest(void)
{
    u32 from = 0;
    for (u32 n = FFT_QUARTER, s = 4; n >= 4; n /= 4, s *= 4) {
        const float *xr = fft_re[from], *xi = fft_im[from];
        float *yr = fft_re[from ^ 1u], *yi = fft_im[from ^ 1u];
        u32 n1 = n / 4;
        for (u32 p = 0; p < n1; ++p) {
            float32x2_t w1 = vld1_f32(twiddle[p * s]), w2 = vld1_f32(twiddle[2 * p * s]), w3 = vld1_f32(twiddle[3 * p * s]);
            for (u32 q = 0; q < s; q += 4) {
                const float *ar = xr + q + s * p, *ai = xi + q + s * p;
                u32 quarter = s * n1;
                BUTTERFLY(vld1q_f32(ar), vld1q_f32(ai), vld1q_f32(ar + quarter), vld1q_f32(ai + quarter),
                          vld1q_f32(ar + 2 * quarter), vld1q_f32(ai + 2 * quarter),
                          vld1q_f32(ar + 3 * quarter), vld1q_f32(ai + 3 * quarter));
                if (p) { turn_lane(&y1r, &y1i, w1); turn_lane(&y2r, &y2i, w2); turn_lane(&y3r, &y3i, w3); }
                float *o = yr + q + s * 4 * p, *oi = yi + q + s * 4 * p;
                vst1q_f32(o, y0r); vst1q_f32(o + s, y1r); vst1q_f32(o + 2 * s, y2r); vst1q_f32(o + 3 * s, y3r);
                vst1q_f32(oi, y0i); vst1q_f32(oi + s, y1i); vst1q_f32(oi + 2 * s, y2i); vst1q_f32(oi + 3 * s, y3i);
            }
        }
        from ^= 1u;
    }
    spectrum_re = fft_re[from]; spectrum_im = fft_im[from];
}

/* |X[k]| of the real FFT from the half-size complex one Z, bins k and
 * FFT_HALF - k together: with E = (Z[k] + conj Z[N-k]) / 2 and
 * P = w^k (Z[k] - conj Z[N-k]) / 2i, X[k] = E + P and X[N-k] = conj(E - P). */
/* 2E and 2P of bins k..k+3: the halves are in the band weights. */
static inline void split(const float *zr, const float *zi, u32 k, float32x4_t *er, float32x4_t *ei, float32x4_t *pr, float32x4_t *pi)
{
    float32x4_t ar = vld1q_f32(zr + k), ai = vld1q_f32(zi + k);
    float32x4_t br = reverse4(vld1q_f32(zr + FFT_HALF - 3 - k)), bj = reverse4(vld1q_f32(zi + FFT_HALF - 3 - k));
    *er = vaddq_f32(ar, br); *ei = vsubq_f32(ai, bj);
    float32x4_t orr = vaddq_f32(ai, bj), oi = vsubq_f32(br, ar);
    float32x4_t wr = vld1q_f32(split_re + k), wi = vld1q_f32(split_im + k);
    *pr = vmlsq_f32(vmulq_f32(wr, orr), wi, oi); *pi = vmlaq_f32(vmulq_f32(wr, oi), wi, orr);
}

static inline float32x4_t norm(float32x4_t r, float32x4_t i) { return square_root(vmlaq_f32(vmulq_f32(r, r), i, i)); }

/* 2|X[k]| of the real FFT from the half-size complex FFT. */
static void magnitudes(void)
{
    float *zr = (float *)spectrum_re, *zi = (float *)spectrum_im;
    zr[FFT_HALF] = zr[0]; zi[FFT_HALF] = zi[0];
    u32 paired = FFT_HALF - 3 < job.bins_used ? 0 : (FFT_HALF - 3 - job.bins_used) & ~3u; /* the first k whose mirror a band reads */
    if (paired > FFT_HALF / 2) paired = FFT_HALF / 2;
    u32 k = 0;
    for (; k < paired; k += 4) {
        float32x4_t er, ei, pr, pi;
        split(zr, zi, k, &er, &ei, &pr, &pi);
        vst1q_f32(magnitude + k, norm(vaddq_f32(er, pr), vaddq_f32(ei, pi)));
    }
    for (; k < FFT_HALF / 2; k += 4) {
        float32x4_t er, ei, pr, pi;
        split(zr, zi, k, &er, &ei, &pr, &pi);
        vst1q_f32(magnitude + k, norm(vaddq_f32(er, pr), vaddq_f32(ei, pi)));
        vst1q_f32(magnitude + FFT_HALF - 3 - k, reverse4(norm(vsubq_f32(er, pr), vsubq_f32(ei, pi))));
    }
    /* bin FFT_HALF / 2: X = conj Z */
    float32x4_t r = vld1q_dup_f32(zr + FFT_HALF / 2), i = vld1q_dup_f32(zi + FFT_HALF / 2);
    vst1q_lane_f32(magnitude + FFT_HALF / 2, norm(vaddq_f32(r, r), vaddq_f32(i, i)), 0);
}

/* One band's weighted sum, as four partial sums. */
static inline float32x4_t band_sum(u32 b)
{
    const float *w = band_weights + band_offset[b], *x = magnitude + band_first[b];
    float32x4_t even = vdupq_n_f32(0.0f), odd = even;  /* two chains: a multiply-add's latency is long */
    u32 i = 0, count = band_count[b];
    for (; i + 8 <= count; i += 8) {
        even = vmlaq_f32(even, vld1q_f32(w + i), vld1q_f32(x + i));
        odd = vmlaq_f32(odd, vld1q_f32(w + i + 4), vld1q_f32(x + i + 4));
    }
    if (i < count) even = vmlaq_f32(even, vld1q_f32(w + i), vld1q_f32(x + i));
    return vaddq_f32(even, odd);
}

static inline float32x2_t pair_total(float32x4_t a, float32x4_t b)
{
    return vpadd_f32(vpadd_f32(vget_low_f32(a), vget_high_f32(a)), vpadd_f32(vget_low_f32(b), vget_high_f32(b)));
}

/* log(1 + gamma x) of each band's sum, four bands at a time in registers:
 * a NEON load of what NEON just stored waits for the store to reach L2. */
static void band_logs(float *now)
{
    float32x4_t gamma = vdupq_n_f32(LOG_GAMMA), one = vdupq_n_f32(1.0f);
    for (u32 b = 0; b < BANDS; b += 4) {
        float32x4_t total = vcombine_f32(pair_total(band_sum(b), band_sum(b + 1)), pair_total(band_sum(b + 2), band_sum(b + 3)));
        vst1q_f32(now + 1 + b, logarithm(vmlaq_f32(one, gamma, total)));
    }
    now[0] = now[1]; now[BANDS + 1] = now[BANDS];
}

/* The SuperFlux sum: each band's rise over the largest of its neighbours a
 * frame earlier. */
static void band_flux(u32 t, const float *now, float *before)
{
    if (!t) memcpy(before, now, sizeof(bands[0]));
    float32x4_t sum = vdupq_n_f32(0.0f), zero = vdupq_n_f32(0.0f);
    for (u32 b = 0; b < BANDS; b += 4) {
        float32x4_t reference = vmaxq_f32(vmaxq_f32(vld1q_f32(before + b), vld1q_f32(before + b + 1)), vld1q_f32(before + b + 2));
        sum = vaddq_f32(sum, vmaxq_f32(vsubq_f32(vld1q_f32(now + 1 + b), reference), zero));
    }
    float32x2_t pair = vadd_f32(vget_low_f32(sum), vget_high_f32(sum));
    vst1_lane_f32(strength + t, vpadd_f32(pair, pair), 0);
}

static void band_strength(u32 t)
{
    float *now = bands[t & 1u], *before = bands[(t & 1u) ^ 1u];
    band_logs(now);
    band_flux(t, now, before);
}

static float block_rms(u32 b)
{
    u32 start = b * job.block, end = start + job.block;
    if (end > job.frames) end = job.frames;
    float32x4_t acc = vdupq_n_f32(0.0f), scale = vdupq_n_f32(job.scale);
    u32 at = start;
    for (; at + 4 <= end; at += 4) {
        float32x4_t x = frames4(job.left + at);
        if (job.right) x = vaddq_f32(x, frames4(job.right + at));
        x = vmulq_f32(x, scale);
        acc = vmlaq_f32(acc, x, x);
    }
    float32x2_t pair = vadd_f32(vget_low_f32(acc), vget_high_f32(acc));
    float sum = vget_lane_f32(vpadd_f32(pair, pair), 0);
    for (; at < end; ++at) {
        float x = (float)job.left[at];
        if (job.right) x += (float)job.right[at];
        x *= job.scale;
        sum += x * x;
    }
    float32x2_t mean = vdup_n_f32(sum / job.block > 0.0f ? sum / job.block : 0.0f);
    float32x2_t e = vrsqrte_f32(vmax_f32(mean, vdup_n_f32(1e-30f)));
    e = vmul_f32(e, vrsqrts_f32(vmul_f32(mean, e), e));
    e = vmul_f32(e, vrsqrts_f32(vmul_f32(mean, e), e));
    return vget_lane_f32(vmul_f32(mean, e), 0);
}

/* The envelope's blocks whose samples this frame has read. */
static void envelope_frame(u32 t)
{
    u32 end = t * job.hop + FFT_HALF;
    if (end > job.frames) end = job.frames + job.block - 1u;
    u32 last = end / job.block;
    if (last > job.blocks) last = job.blocks;
    if (last > ENVELOPE_MAX) last = ENVELOPE_MAX;
    while (job.envelope_blocks < last) { envelope[job.envelope_blocks] = block_rms(job.envelope_blocks); ++job.envelope_blocks; }
}

static void strength_frame(u32 t)
{
    envelope_frame(t);
    first_pass(t);
    fft_rest();
    magnitudes();
    band_strength(t);
}

/* ---- peaks ---- */

/* The value at rank k of v[0..n) (0-based), by selection with a three-way
 * partition (the strength is mostly zeros); v is reordered. */
static float rank(float *v, u32 n, u32 k)
{
    u32 lo = 0, hi = n;
    for (;;) {
        float pivot = v[lo + (hi - lo) / 2];
        u32 less = lo, at = lo, more = hi;
        while (at < more) {
            float x = v[at];
            if (x < pivot) { v[at++] = v[less]; v[less++] = x; }
            else if (x > pivot) { v[at] = v[--more]; v[more] = x; }
            else ++at;
        }
        if (k < less) hi = less;
        else if (k >= more) lo = more;
        else return pivot;
    }
}

/* numpy's linear percentile of the strength. */
static double percentile(u32 n)
{
    memcpy(scratch.order, strength, n * sizeof(float));
    double at = PEAK_PERCENTILE * (n - 1);
    u32 k = (u32)at;
    double low = rank(scratch.order, n, k);
    if (k + 1 >= n) return low;
    float high = scratch.order[k + 1];
    for (u32 i = k + 2; i < n; ++i) if (scratch.order[i] < high) high = scratch.order[i];
    return low + (at - k) * (high - low);
}

static int local_max(u32 n, u32 at, u32 reach)
{
    u32 lo = at > reach ? at - reach : 0, hi = at + reach < n ? at + reach : n - 1;
    for (u32 i = lo; i <= hi; ++i) if (strength[i] > strength[at]) return 0;
    return 1;
}

static void pick(void)
{
    u32 n = job.strength_frames;
    double p95 = percentile(n), frame_ms = 1000.0 * job.hop / job.rate;
    u32 hit_reach = round_even(HIT_MAX_MS / frame_ms), rise_reach = round_even(RISE_MAX_MS / frame_ms);
    u32 avg = round_even(PEAK_AVG_MS / frame_ms);
    job.candidates = 0;
    for (u32 t = 0; t < n && job.candidates < CANDIDATES_MAX; ++t) {
        double s = strength[t];
        if (s <= 0.0 || !local_max(n, t, rise_reach)) continue;
        double mean = 0.0;
        for (s32 j = -(s32)avg; j <= (s32)avg; ++j) {
            s32 i = (s32)t + j;
            mean += strength[i < 0 ? 0 : i >= (s32)n ? n - 1 : (u32)i];
        }
        mean /= 2 * avg + 1;
        if (s < mean + RISE_DELTA * p95) continue;
        u32 hit = s >= mean + HIT_DELTA * p95 && local_max(n, t, hit_reach);
        candidate[job.candidates++] = t | (hit ? HIT_FLAG : 0u);
    }
}

/* ---- attacks ---- */

static inline s64 mono_sum(u32 at) { return (s64)job.left[at] + (job.right ? job.right[at] : 0); }

/* The zero crossing before the attack near sample `at`. */
static u32 attack(u32 at)
{
    s32 centre = (s32)(at / job.block);
    s32 lo = centre - ATTACK_BLOCKS, hi = centre + ATTACK_BLOCKS;
    if (lo < 0) lo = 0;
    if (hi > (s32)job.envelope_blocks - 1) hi = (s32)job.envelope_blocks - 1;
    u32 sample = at;
    if (hi > lo + 1) {
        const float *seg = envelope + lo;
        u32 count = (u32)(hi - lo + 1), steep = 0;
        float best = seg[1] - seg[0];
        for (u32 i = 1; i + 1 < count; ++i) if (seg[i + 1] - seg[i] > best) { best = seg[i + 1] - seg[i]; steep = i; }
        float floor = seg[0], peak = seg[steep + 1];
        for (u32 i = 1; i <= steep; ++i) if (seg[i] < floor) floor = seg[i];
        for (u32 i = steep + 2; i < count; ++i) if (seg[i] > peak) peak = seg[i];
        float level = floor + (float)ATTACK_FRACTION * (peak - floor);
        u32 a = steep;
        while (a > 0 && seg[a] > level) --a;
        sample = ((u32)lo + a) * job.block;
    }
    if (sample > job.frames - 1) sample = job.frames - 1;
    u32 start = sample > ZERO_BLOCKS * job.block ? sample - ZERO_BLOCKS * job.block : 0;
    for (u32 i = sample; i > start; --i)
        if ((mono_sum(i - 1) < 0) != (mono_sum(i) < 0)) return i;
    return sample;
}

/* Sorted, one entry a position; a position some hit candidate reached keeps
 * the hit flag. */
static void finish(void)
{
    u32 n = job.results;
    for (u32 i = 1; i < n; ++i) {
        u32 v = result[i], j = i;
        while (j && (result[j - 1] & ~HIT_FLAG) > (v & ~HIT_FLAG)) { result[j] = result[j - 1]; --j; }
        result[j] = v;
    }
    u32 kept = 0;
    for (u32 i = 0; i < n; ++i) {
        if (kept && (result[kept - 1] & ~HIT_FLAG) == (result[i] & ~HIT_FLAG)) result[kept - 1] |= result[i];
        else result[kept++] = result[i];
    }
    job.results = kept;
}

/* ---- the job ---- */

void hits_begin(const s32 *left, const s32 *right, u32 frames, u32 rate)
{
    if (!job.table_rate) fft_tables();
    if (job.table_rate != rate) band_tables(rate);
    job.left = left; job.right = right; job.frames = frames; job.rate = rate;
    job.hop = round_even(rate * (double)HOP_MS / 1000.0);
    job.block = rate / BLOCK_HZ;
    job.blocks = (frames + job.block - 1) / job.block;
    job.scale = right ? 0.5f * Q31_SCALE : Q31_SCALE;
    job.strength_frames = 1 + (frames + FFT_HALF) / job.hop;
    if (job.strength_frames > STRENGTH_MAX) job.strength_frames = STRENGTH_MAX;
    job.next = job.results = job.envelope_blocks = 0;
    job.stage = frames > FFT_POINTS ? STAGE_STRENGTH : STAGE_DONE;
}

/* One step; nonzero once the job is done. */
int hits_step(void)
{
    switch (job.stage) {
    case STAGE_STRENGTH:
        strength_frame(job.next);
        if (++job.next == job.strength_frames) job.stage = STAGE_PICK;
        return 0;
    case STAGE_PICK:
        pick();
        job.next = 0;
        job.stage = STAGE_ATTACK;
        return 0;
    case STAGE_ATTACK:
        if (job.next < job.candidates) {
            u32 c = candidate[job.next++];
            result[job.results++] = attack((c & ~HIT_FLAG) * job.hop) | (c & HIT_FLAG);
            return 0;
        }
        finish();
        job.stage = STAGE_DONE;
        return 1;
    }
    return 1;
}

/* The finished job's positions, HIT_FLAG on those that start slices, and the
 * frames they cover. */
u32 hits_take(u32 *out, u32 room, u32 *analysed)
{
    u32 n = job.results < room ? job.results : room;
    memcpy(out, result, n * sizeof(u32));
    u32 covered = job.strength_frames * job.hop;
    if (covered > job.frames) covered = job.frames;
    if (n < job.results) covered = out[n - 1] & ~HIT_FLAG;
    *analysed = covered;
    return n;
}
