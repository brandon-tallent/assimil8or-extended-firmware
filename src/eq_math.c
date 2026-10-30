#include "eq.h"
#include "eq_design.h"
/* Foreground-only VFP math. The stock IRQ entry saves every D register and
 * FPSCR, including on nested interrupts. No libm or soft-float helpers. */
double sine(double x)
{
    double term = x, sum = x, square = -x*x;
    for (u32 n = 2; n <= 20; n += 2) { term *= square / (n*(n+1)); sum += term; }
    return sum;
}
double cosine(double x) { return sine(1.5707963267948966 - x); }
double exponential(double x)
{
    double term = 1, sum = 1;
    for (u32 n = 1; n <= 32; ++n) { term *= x/n; sum += term; }
    return sum;
}
static double step_hz(u32 step); /* a frequency step in Hz */
static double square_root(double x)
{
    double y = 1;
    for (u32 n = 0; n < 8; ++n) y = (y + x/y)*0.5;
    return y;
}
static void normalize(s32 *out, double b0, double b1, double b2,
                      double a0, double a1, double a2)
{
    double row[5] = { b0/a0, b1/a0, b2/a0, -a1/a0, -a2/a0 };
    for (u32 n = 0; n < 5; ++n) {
        double x = row[n]*EQ_UNITY;
        out[n] = (s32)(x + (x < 0 ? -0.5 : 0.5));
    }
}
s32 eq_gain_limit(const struct eq_value *value, u32 band)
{
    return band == EQ_BAND_MID || value->q[band] ? EQ_GAIN_MAX : EQ_SHELF_GAIN_MAX;
}
void eq_limit(struct eq_value *value)
{
    for (u32 band = 0; band < 3; ++band) {
        s32 limit = eq_gain_limit(value, band);
        if (value->gain[band] > limit) value->gain[band] = (signed short)limit;
        if (value->gain[band] < -limit) value->gain[band] = (signed short)-limit;
    }
}
static double bell_q(u32 q)
{
    return 0.5*exponential((double)(q-1)*(0.6931471805599453/EQ_Q_STEPS_PER_OCTAVE));
}
u32 eq_q_hundredths(u32 q) { return (u32)(bell_q(q)*100+0.5); }
void eq_coefficients(const struct eq_value *value, s32 out[5][5])
{
    for (u32 s = 0; s < 5; ++s) {
        for (u32 c = 0; c < 5; ++c) out[s][c] = c ? 0 : EQ_UNITY;
    }
    if (!value->on) return;
    if (value->hpf) {
        double w = value->hpf * (6.283185307179586/48000), c = cosine(w), a = sine(w)*0.5;
        normalize(out[0], (1+c)*0.5, -(1+c), (1+c)*0.5, 1+a, -2*c, 1-a);
        double t = sine(w*0.5)/cosine(w*0.5);
        normalize(out[1], 1, -1, 0, 1+t, t-1, 0);
    }
    for (u32 band = 0; band < 3; ++band) {
        if (!value->gain[band]) continue;
        double a = exponential(value->gain[band]*(2.302585092994046/400));
        double w = step_hz(value->frequency[band])*(6.283185307179586/48000);
        double c = cosine(w), s = sine(w), p = a+1, m = a-1, b = s*square_root(2*a);
        if (band == EQ_BAND_MID || value->q[band]) {
            /* The Mid band's own Q is broad at small gain, narrower at
             * larger boost/cut. This is a provisional proportional-Q law,
             * not a measured Overstayer fit. */
            double q = value->q[band] ? bell_q(value->q[band]) : 0.5*(a >= 1 ? a*a : 1/(a*a));
            double alpha = s/(2*q);
            normalize(out[2+band], 1+alpha*a, -2*c, 1-alpha*a, 1+alpha/a, -2*c, 1-alpha/a);
        } else if (band == 0)
            normalize(out[2], a*(p-m*c+b), 2*a*(m-p*c), a*(p-m*c-b), p+m*c+b, -2*(m+p*c), p+m*c-b);
        else
            normalize(out[4], a*(p+m*c+b), -2*a*(m+p*c), a*(p+m*c-b), p-m*c+b, 2*(m-p*c), p-m*c-b);
    }
}
/* Natural log, positive normal doubles only. Mantissa reduction bounds the
 * atanh series argument to 1/3. Used once per changed response graph. */
double logarithm(double x)
{
    union { double d; u32 word[2]; } number = { .d = x };
    s32 exponent = (s32)((number.word[1] >> 20) & 2047) - 1023;
    number.word[1] = (number.word[1] & 0xfffff) | 0x3ff00000;
    double z = (number.d-1)/(number.d+1), term = z, sum = z;
    for (u32 n = 3; n <= 21; n += 2) { term *= z*z; sum += term/n; }
    return exponent*0.6931471805599453 + 2*sum;
}
#define LN2 0.6931471805599453
#define CENTIHERTZ_PER_HZ 100
#define EQ_CENTS_SPAN 12000u /* past 20 kHz from EQ_FREQUENCY_MIN */
static double step_hz(u32 step)
{
    if (step > EQ_STEP_MAX) step = EQ_STEP_MAX;
    double hz = EQ_STEP_HZ_1K*exponential(((s32)step - EQ_STEP_1K)*(LN2/EQ_STEPS_PER_OCTAVE));
    return hz > EQ_FREQUENCY_MAX ? EQ_FREQUENCY_MAX : hz;
}
static u32 nearest(double hz)
{
    double step = EQ_STEP_1K + logarithm(hz*(1.0/EQ_STEP_HZ_1K))*(EQ_STEPS_PER_OCTAVE/LN2);
    return step <= 0 ? 0 : step >= EQ_STEP_MAX ? EQ_STEP_MAX : (u32)(step + 0.5);
}
u32 eq_step_centihertz(u32 step) { return (u32)(step_hz(step)*CENTIHERTZ_PER_HZ + 0.5); }
u32 eq_step_nearest(u32 centihertz) { return centihertz ? nearest(centihertz*(1.0/CENTIHERTZ_PER_HZ)) : 0; }
u32 eq_step_above_min(u32 cents)
{
    if (cents > EQ_CENTS_SPAN) cents = EQ_CENTS_SPAN; /* the series stays exact */
    return nearest(EQ_FREQUENCY_MIN*exponential(cents*(LN2/EQ_CENTS_PER_OCTAVE)));
}
/* The graphs' logarithmic 20 Hz..20 kHz scale. */
float eq_frequency_position(float hz)
{
    return (float)(logarithm(hz*(1.0/EQ_FREQUENCY_MIN))/logarithm((double)EQ_FREQUENCY_MAX/EQ_FREQUENCY_MIN));
}
float eq_step_position(u32 step) { return eq_frequency_position((float)step_hz(step)); }
u16 eq_frequency_shift(u16 frequency, float octaves, u16 low, u16 high)
{
    double shifted = frequency*exponential(octaves*LN2);
    return shifted <= low ? low : shifted >= high ? high : (u16)(shifted+0.5);
}
/* Response in dB mapped to rows top..bottom (+/-EQ_GRAPH_DB about the
 * middle), sampled at points logarithmically spaced from 20 Hz to 20 kHz. */
void eq_curve(const struct eq_value *value, u32 points, u8 *out, s32 top, s32 bottom)
{
    s32 coefficients[5][5];
    eq_coefficients(value, coefficients);
    double frequency = EQ_FREQUENCY_MIN, multiplier = exponential(6.907755278982137/(points-1));
    s32 middle = (top+bottom)/2;
    for (u32 n = 0; n < points; ++n, frequency *= multiplier) {
        double w = frequency*(6.283185307179586/48000), c = cosine(w), s = sine(w);
        double c2 = 2*c*c-1, s2 = 2*s*c, magnitude = 1;
        for (u32 section = 0; section < 5; ++section) {
            double row[5];
            for (u32 k = 0; k < 5; ++k) row[k] = (double)coefficients[section][k]/EQ_UNITY;
            double br = row[0]+row[1]*c+row[2]*c2, bi = row[1]*s+row[2]*s2;
            double ar = 1-row[3]*c-row[4]*c2, ai = row[3]*s+row[4]*s2;
            magnitude *= (br*br+bi*bi)/(ar*ar+ai*ai);
        }
        if (magnitude < 1e-12) magnitude = 1e-12;
        double decibels = logarithm(magnitude)*(10/2.302585092994046);
        double rows = decibels*(bottom-middle)/EQ_GRAPH_DB;
        s32 y = middle - (s32)(rows + (rows < 0 ? -0.5 : 0.5));
        out[n] = (u8)(y < top ? top : y > bottom ? bottom : y);
    }
}

/* The channel filter (filter.S; the model is tools/svf_model.py). A section's
 * trapezoidal coefficients follow from g = tan(pi f / fs) and the damping
 * k = 1/Q. A filter type chooses the taps of section 1 that feed section 2
 * (mix 1) and the taps of section 2 and its input, and section 1's lowpass,
 * that reach the output (mix 2). Ahead of the sections a type may shape the
 * input, hold it, or modulate section 1's cutoff by another channel.
 *
 * The types, their levels, the resonance law and Extra's laws follow the
 * Microwave XT's Filter 1 as rendered on its emulated firmware: resonance
 * takes the damping from 2 (Q 0.5) toward 0 as 2(1 - r)^1.35; the 24 dB
 * types resonate in one section and hold the other at 2; and Extra (Spread, Offset) moves
 * Band Stop's and Dual L/BP's second filter by 2^(steps / 9.06), the XT's
 * 11.5 steps an octave on its 0..127 scale. */
#define FILTER_DAMPING_MAX 2.0           /* resonance 0: critically damped */
#define FILTER_DAMPING_MIN 0.05          /* resonance 100: Q 20 */
#define FILTER_DAMPING_POWER 1.35
#define FILTER_DAMPING_FIXED 2.0         /* the 24 dB types' second section */
#define FILTER_EXTRA_OCTAVES_PER_STEP 0.1104 /* 127 / (100 * 11.5) */
#define FILTER_DUAL_CENTRE 50.4          /* Extra at which Dual's band-pass sits on the cutoff */
#define Q30_SCALE 1073741824.0
#define Q31_SCALE 2147483648.0
#define Q32_SCALE 4294967296.0
const char *filter_extra_name(u32 type)
{
    switch (type) {
    case FILTER_BAND_STOP: return "SPREAD";
    case FILTER_DUAL: return "OFFSET";
    default: return 0;
    }
}
void filter_design(const struct filter_value *v, struct filter_design *d)
{
    memset(d, 0, sizeof(*d));
    double r = v->resonance*(1.0/FILTER_RESONANCE_MAX);
    double k = r < 1 ? FILTER_DAMPING_MAX*exponential(FILTER_DAMPING_POWER*logarithm(1 - r)) : 0;
    if (k < FILTER_DAMPING_MIN) k = FILTER_DAMPING_MIN;
    d->cutoff[0] = d->cutoff[1] = step_hz(v->cutoff);
    d->damping[0] = d->damping[1] = k;
    switch (v->type) {
    case FILTER_LP24:
        d->mix1[MIX_LP] = 1; d->mix2[MIX_LP] = 1; d->damping[1] = FILTER_DAMPING_FIXED;
        break;
    case FILTER_LP12: d->mix1[MIX_LP] = 1; d->mix2[MIX_INPUT] = 1; break;
    case FILTER_BP24:
        /* A band-pass into a fixed low-pass; the XT's level, 2.1, split between them. */
        d->mix1[MIX_BP] = 1.45; d->mix2[MIX_LP] = 1.45; d->damping[1] = FILTER_DAMPING_FIXED;
        break;
    case FILTER_BP12: d->mix1[MIX_BP] = 1.8; d->mix2[MIX_INPUT] = 1; break;
    case FILTER_HP24:
        d->mix1[MIX_HP] = 1; d->mix2[MIX_HP] = 1; d->damping[1] = FILTER_DAMPING_FIXED;
        break;
    case FILTER_HP12: d->mix1[MIX_HP] = 0.95; d->mix2[MIX_INPUT] = 1; break;
    case FILTER_NOTCH24:
        /* A sharp notch into a broad one. */
        d->mix1[MIX_LP] = d->mix1[MIX_HP] = 1; d->mix2[MIX_LP] = d->mix2[MIX_HP] = 1.15;
        d->damping[0] = k*0.5; d->damping[1] = FILTER_DAMPING_FIXED + 0.85*k;
        break;
    case FILTER_NOTCH12: d->mix1[MIX_LP] = d->mix1[MIX_HP] = 0.51; d->mix2[MIX_INPUT] = 1; break;
    case FILTER_BAND_STOP:
        /* A low-pass on the cutoff beside a high-pass Spread above it. */
        d->mix1[MIX_INPUT] = 1; d->mix2[MIX_LP1] = 0.5; d->mix2[MIX_HP] = 0.55;
        d->damping[0] = k*0.5; d->damping[1] = k*1.25 < 1.1 ? k*1.25 : 1.1;
        d->cutoff[1] = d->cutoff[0]*exponential(v->extra*FILTER_EXTRA_OCTAVES_PER_STEP*0.6931471805599453);
        break;
    case FILTER_DUAL:
        /* A low-pass on the cutoff beside a band-pass that Offset moves either way. */
        d->damping[0] = k*0.5; d->damping[1] = k*1.25 < 1.1 ? k*1.25 : 1.1;
        d->mix1[MIX_INPUT] = 1; d->mix2[MIX_LP1] = 0.5; d->mix2[MIX_BP] = 0.5*d->damping[1];
        d->cutoff[1] = d->cutoff[0]*exponential((v->extra - FILTER_DUAL_CENTRE)*FILTER_EXTRA_OCTAVES_PER_STEP*0.6931471805599453);
        break;
    default: break;
    }
}
static s32 q30(double x)
{
    x *= Q30_SCALE;
    return x >= 2147483647.0 ? 2147483647 : x <= -2147483648.0 ? (s32)-2147483648.0 : (s32)(x + (x < 0 ? -0.5 : 0.5));
}
static s32 q31(double x)
{
    x *= Q31_SCALE;
    return x >= 2147483647.0 ? 2147483647 : x <= -2147483648.0 ? (s32)-2147483648.0 : (s32)(x + (x < 0 ? -0.5 : 0.5));
}
static double section_g(double cutoff)
{
    double x = (cutoff > EQ_FREQUENCY_MAX ? EQ_FREQUENCY_MAX : cutoff)*(3.141592653589793/48000);
    return sine(x)/cosine(x);
}
static void filter_section(s32 *out, double cutoff, double damping)
{
    double g = section_g(cutoff), a1 = 1/(1 + g*(g + damping)), a2 = g*a1;
    out[0] = q31(a1); out[1] = q31(a2); out[2] = q31(g*a2); out[3] = q31(damping*0.25);
}
static s32 float_bits(float x)
{
    union { float f; s32 bits; } u = { .f = x };
    return u.bits;
}
/* One lane's vectors, a word each: the caller stores them into its lane. */
void filter_lane(const struct filter_value *v, s32 out[FILTER_VECTORS])
{
    struct filter_design d;
    filter_design(v, &d);
    memset(out, 0, FILTER_VECTORS*sizeof(s32));
    out[FILTER_VECTOR_MASK] = v->type == FILTER_OFF ? 0 : -1;
    filter_section(&out[FILTER_VECTOR_SECTION_1], d.cutoff[0], d.damping[0]);
    filter_section(&out[FILTER_VECTOR_SECTION_2], d.cutoff[1], d.damping[1]);
    for (u32 n = 0; n < 4; ++n) {
        out[FILTER_VECTOR_MIX_1 + n] = q30(d.mix1[n]);
        out[FILTER_VECTOR_MIX_2 + n] = q30(d.mix2[n]);
    }
    out[FILTER_VECTOR_MIX_2_LP1] = q30(d.mix2[MIX_LP1]);
    /* A lane whose cutoff CV moves it has its coefficients worked out each
     * frame from these bases; the caller sets the CV and the mask (eq_model.c). */
    out[FILTER_VECTOR_G1] = float_bits((float)section_g(d.cutoff[0]));
    out[FILTER_VECTOR_G2] = float_bits((float)section_g(d.cutoff[1]));
    out[FILTER_VECTOR_K1] = float_bits((float)d.damping[0]);
    out[FILTER_VECTOR_K2] = float_bits((float)d.damping[1]);
}
