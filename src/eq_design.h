/* The filter's design and the maths it uses (eq_math.c), shared with the
 * extension's filter graph (ext/filter_curve.c). */
#ifndef EQ_DESIGN_H
#define EQ_DESIGN_H
#include "eq.h"

enum { MIX_INPUT, MIX_LP, MIX_BP, MIX_HP, MIX_LP1, MIX_WEIGHTS };
struct filter_design {
    double cutoff[2], damping[2], mix1[4], mix2[MIX_WEIGHTS]; /* mix1 has no MIX_LP1 */
};

void filter_design(const struct filter_value *v, struct filter_design *d);
double sine(double x);
double cosine(double x);
double exponential(double x);
double logarithm(double x);

#endif
