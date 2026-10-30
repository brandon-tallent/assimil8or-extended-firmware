#include "ext.h"
#include "eq_design.h"

/* The filter page's graph: the type's response in dB. Built for VFP, as
 * the overlay's filter maths it calls. */

/* A section's taps at frequency w (radians per sample): with s = j*t the
 * lowpass is 1/d, the bandpass s/d and the highpass s*s/d, d = s*s + k*s + 1. */
static void filter_taps(double cutoff, double damping, double w, double lp[2], double bp[2], double hp[2])
{
    double x = (cutoff > EQ_FREQUENCY_MAX ? EQ_FREQUENCY_MAX : cutoff)*(3.141592653589793/48000);
    double t = (sine(w*0.5)/cosine(w*0.5))*cosine(x)/sine(x);
    double dr = 1 - t*t, di = damping*t, n = dr*dr + di*di;
    lp[0] = dr/n; lp[1] = -di/n;
    bp[0] = di*t/n; bp[1] = dr*t/n;                 /* j*t / d */
    hp[0] = -t*t*dr/n; hp[1] = t*t*di/n;            /* -t*t / d */
}
/* The type's response in dB, rows top..bottom spanning db_top..db_bottom,
 * at points logarithmically spaced from 20 Hz to 20 kHz. */
void filter_curve(const struct filter_value *value, u32 points, u8 *out, s32 top, s32 bottom, s32 db_top, s32 db_bottom)
{
    struct filter_design d;
    filter_design(value, &d);
    double frequency = EQ_FREQUENCY_MIN, multiplier = exponential(6.907755278982137/(points-1));
    for (u32 n = 0; n < points; ++n, frequency *= multiplier) {
        double w = frequency*(6.283185307179586/48000);
        double a[2][2] = {{d.mix1[MIX_INPUT], 0}, {d.mix2[MIX_INPUT], 0}}, lp1[2] = {0, 0};
        for (u32 section = 0; section < 2; ++section) {
            double lp[2], bp[2], hp[2];
            filter_taps(d.cutoff[section], d.damping[section], w, lp, bp, hp);
            const double *m = section ? d.mix2 : d.mix1;
            for (u32 part = 0; part < 2; ++part) {
                a[section][part] += m[MIX_LP]*lp[part] + m[MIX_BP]*bp[part] + m[MIX_HP]*hp[part];
                if (!section) lp1[part] = lp[part];
            }
        }
        /* Section 2 after section 1, plus section 1's lowpass beside it. */
        double re = a[0][0]*a[1][0] - a[0][1]*a[1][1] + d.mix2[MIX_LP1]*lp1[0];
        double im = a[0][0]*a[1][1] + a[0][1]*a[1][0] + d.mix2[MIX_LP1]*lp1[1];
        double magnitude = re*re + im*im;
        if (magnitude < 1e-12) magnitude = 1e-12;
        double decibels = logarithm(magnitude)*(10/2.302585092994046);
        double rows = (db_top - decibels)*(bottom - top)/(db_top - db_bottom);
        s32 y = top + (s32)(rows + (rows < 0 ? -0.5 : 0.5));
        out[n] = (u8)(y < top ? top : y > bottom ? bottom : y);
    }
}
