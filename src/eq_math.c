#include "eq.h"
/* Foreground-only VFP math. The stock IRQ entry saves every D register and
 * FPSCR, including on nested interrupts. No libm or soft-float helpers. */
static double sine(double x)
{
    double term = x, sum = x, square = -x*x;
    for (u32 n = 2; n <= 20; n += 2) { term *= square / (n*(n+1)); sum += term; }
    return sum;
}
static double cosine(double x) { return sine(1.5707963267948966 - x); }
static double exponential(double x)
{
    double term = 1, sum = 1;
    for (u32 n = 1; n <= 32; ++n) { term *= x/n; sum += term; }
    return sum;
}
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
        double w = value->frequency[band]*(6.283185307179586/48000);
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
static double logarithm(double x)
{
    union { double d; u32 word[2]; } number = { .d = x };
    s32 exponent = (s32)((number.word[1] >> 20) & 2047) - 1023;
    number.word[1] = (number.word[1] & 0xfffff) | 0x3ff00000;
    double z = (number.d-1)/(number.d+1), term = z, sum = z;
    for (u32 n = 3; n <= 21; n += 2) { term *= z*z; sum += term/n; }
    return exponent*0.6931471805599453 + 2*sum;
}
/* Band frequencies share one logarithmic 20 Hz..20 kHz position scale. */
u16 eq_frequency(float position)
{
    if (position <= 0) return EQ_FREQUENCY_MIN;
    if (position >= 1) return EQ_FREQUENCY_MAX;
    return (u16)(EQ_FREQUENCY_MIN*exponential(position*logarithm((double)EQ_FREQUENCY_MAX/EQ_FREQUENCY_MIN))+0.5);
}
float eq_frequency_position(u16 frequency)
{
    return (float)(logarithm((double)frequency/EQ_FREQUENCY_MIN)/logarithm((double)EQ_FREQUENCY_MAX/EQ_FREQUENCY_MIN));
}
u16 eq_frequency_step(u16 frequency, s32 delta)
{
    u16 next = eq_frequency(eq_frequency_position(frequency)+(float)delta/EQ_FREQUENCY_STEPS);
    /* Integer-Hz rounding must not trap encoder turns near the low endpoint. */
    if (delta && next == frequency) {
        if (delta > 0 && frequency < EQ_FREQUENCY_MAX) next++;
        if (delta < 0 && frequency > EQ_FREQUENCY_MIN) next--;
    }
    return next;
}
u16 eq_frequency_shift(u16 frequency, float octaves, u16 low, u16 high)
{
    double shifted = frequency*exponential(octaves*0.6931471805599453);
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
