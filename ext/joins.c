#include "ext.h"
#include <arm_neon.h>
#include "timelock.h"

/* Time Lock's join search: the offset and crossfade law for the next join,
 * planned in the foreground by normalised cross-correlation. */

#define MATCH_FRAMES          576u    /* 12 ms */
#define REACH_FRAMES          480u    /* 10 ms either side of the timeline */
#define COARSE                4u
#define ONSET_MATCH_FRAMES    192u    /* 4 ms */
#define ONSET_STRIDE          2u
#define HORIZON_FRAMES        1440u   /* plan an event at most 30 ms ahead */
#define LEAD_FRAMES           4u      /* and at least this far ahead */
#define LAW_FLOOR             0.2f    /* correlation from which the law moves toward equal gain */
#define LAW_SPAN              0.7f    /* and reaches it at LAW_FLOOR + LAW_SPAN */
#define LAW_FULL              256.0f
#define BIAS                  1e-3f   /* a start's handicap per reach of distance from the natural one */
#define TEMPLATE_MAX          (MATCH_FRAMES / COARSE + 4u)
#define REGION_MAX            ((2u * REACH_FRAMES) / COARSE + TEMPLATE_MAX + 4u)
#define ROUND_HALF_Q32        (1ll << 31)

static float template_points[TEMPLATE_MAX] __attribute__((aligned(16)));
static float region[REGION_MAX] __attribute__((aligned(16)));
static float near[TEMPLATE_MAX] __attribute__((aligned(16)));
u32 joins_planned, joins_late; /* plans made; events already too close to plan */

/* ---- correlation ---- */

static inline float sum4(float32x4_t v)
{
    float32x2_t p = vadd_f32(vget_low_f32(v), vget_high_f32(v));
    return vget_lane_f32(vpadd_f32(p, p), 0);
}

static float dot(const float *a, const float *b, u32 n)
{
    float32x4_t s0 = vdupq_n_f32(0.0f), s1 = s0;
    u32 i = 0;
    for (; i + 8 <= n; i += 8) {
        s0 = vmlaq_f32(s0, vld1q_f32(a + i), vld1q_f32(b + i));
        s1 = vmlaq_f32(s1, vld1q_f32(a + i + 4), vld1q_f32(b + i + 4));
    }
    float s = sum4(vaddq_f32(s0, s1));
    for (; i < n; ++i) s += a[i] * b[i];
    return s;
}

static inline float inverse_root(float x)
{
    float32x2_t v = vdup_n_f32(x > 1e-30f ? x : 1e-30f), e = vrsqrte_f32(v);
    e = vmul_f32(e, vrsqrts_f32(vmul_f32(v, e), e));
    e = vmul_f32(e, vrsqrts_f32(vmul_f32(v, e), e));
    return vget_lane_f32(e, 0);
}

static float ncc(const float *a, const float *b, u32 n) { return dot(a, b, n) * inverse_root(dot(a, a, n) * dot(b, b, n)); }

static u8 law_of(float r)
{
    float w = (r - LAW_FLOOR) / LAW_SPAN;
    w = w < 0.0f ? 0.0f : w > 1.0f ? 1.0f : w;
    return (u8)(w * (LAW_FULL - 1.0f));
}

/* ---- reads ---- */

/* The mono sample nearest an unrolled position, the voice in lap `lap`. */
static float sample_at(const struct lock_zone *z, u32 pair, s64 at, s64 lap)
{
    s64 real = at - lap;
    if (z->turn < z->end) {
        s64 round = (s64)(z->turn - z->loop) << 32;
        if (real >= (s64)z->turn << 32) real -= round;
        else if (real < (s64)z->loop << 32 && lap > 0) real += round;
    }
    real += ROUND_HALF_Q32;
    if (real < 0) return 0.0f;
    u32 f = (u32)(real >> 32);
    if (f < z->first || f >= z->end) return 0.0f;
    float v = (float)z->pcm[0][f];
    if (pair && z->pcm[1]) v += (float)z->pcm[1][f];
    return v;
}

static void gather(float *out, u32 n, const struct lock_zone *z, u32 pair, s64 start, s64 stride, s64 lap)
{
    for (u32 i = 0; i < n; ++i) out[i] = sample_at(z, pair, start + (s64)i * stride, lap);
}

/* The last rise after the slice's start and before `at`, as the interrupt's. */
static s64 rise_before(const struct lock *l, const struct lock_zone *z, s64 at)
{
    s64 found = 0;
    for (u32 i = l->cursor; i < z->count && i < l->b_index; ++i) {
        u32 frame = z->positions[i] & ~HIT_FLAG;
        if (frame >= z->turn || ((s64)frame << 32) + l->a_lap >= at) break;
        found = ((s64)frame << 32) + l->a_lap;
    }
    return found;
}

/* ---- plans ---- */

static void publish(struct lock *l, u32 serial, u32 kind, s64 offset, u8 law)
{
    l->plan.key = ~serial;   /* not this event's while it changes */
    MEMORY_BARRIER();
    l->plan.kind = (u8)kind; l->plan.law = law; l->plan.offset = offset;
    MEMORY_BARRIER();
    l->plan.key = serial;
    ++joins_planned;
}

static void plan_onset(struct lock *l, const struct lock *s, const struct lock_zone *z, u32 pair, s64 now, s64 per)
{
    /* The read the onset leaves reaches this far by the hit. */
    s64 old_end = now + (s64)PREROLL_FRAMES * per, stride = (s64)ONSET_STRIDE * per;
    u32 n = ONSET_MATCH_FRAMES / ONSET_STRIDE;
    gather(template_points, n, z, pair, old_end - (s64)n * stride, stride, s->lap);
    gather(near, n, z, pair, s->b - (s64)n * stride, stride, s->lap);
    publish(l, s->serial, PLAN_ONSET, 0, law_of(ncc(template_points, near, n)));
}

static void plan_fill(struct lock *l, const struct lock *s, const struct lock_zone *z, u32 pair, s64 now, s64 nominal, s64 per)
{
    s64 base = (s64)z->base << (32u - AUDIO_STEP_SHIFT);
    u32 left = s->length - (u32)s->due, piece = s->next_piece;
    int last = left < piece + piece / 2;
    u32 reads = last ? left : piece + s->xfade;
    u32 settle = SETTLE_FRAMES < s->own / 3 ? SETTLE_FRAMES : s->own / 3;
    s64 lo = s->a + (s64)settle * per, rise = rise_before(s, z, now);
    if (rise) {
        s64 guard = rise + (s64)PROTECT_FRAMES * base;
        if (guard > lo) lo = guard < now ? guard : now;
    }
    s64 hi = s->b - (s64)reads * per;
    /* Starts on the read's grid: nominal + k per. */
    s64 kmin = -(s64)REACH_FRAMES, kmax = REACH_FRAMES;
    if (nominal + kmin * per < lo) kmin = (lo - nominal + per - 1) / per;
    if (nominal + kmax * per > hi) kmax = (hi - nominal) / per;
    u32 avail = s->own - settle;
    u32 span = MATCH_FRAMES < avail / 2 ? MATCH_FRAMES : avail / 2;
    if (span < s->xfade) span = s->xfade;
    u32 n = span / COARSE;
    if (n > TEMPLATE_MAX) n = TEMPLATE_MAX;
    if (n < 4) return;
    s64 coarse = (s64)COARSE * per;
    gather(template_points, n, z, pair, now, coarse, s->lap);
    if (kmax < kmin) { /* nowhere to search: the interrupt clamps the timeline */
        s64 to = nominal < lo ? lo : nominal > hi ? hi : nominal;
        gather(near, n, z, pair, to, coarse, s->lap);
        publish(l, s->serial, PLAN_FILL, 0, law_of(ncc(template_points, near, n)));
        return;
    }
    s64 natural = (now - nominal) / per; /* the natural start's k */
    u32 candidates = (u32)((kmax - kmin) / COARSE) + 1u;
    if (candidates + n > REGION_MAX) candidates = REGION_MAX - n;
    gather(region, candidates + n, z, pair, nominal + kmin * per, coarse, s->lap);
    float tt = dot(template_points, template_points, n), best = -2.0f, best_r = 1.0f;
    s64 at = natural < kmin ? kmin : natural > kmax ? kmax : natural;
    if (tt > 1e-6f * (float)n) {
        float energy = dot(region, region, n), per_k = BIAS / (float)REACH_FRAMES;
        for (u32 m = 0; m < candidates; ++m) {
            if (m) energy += region[m + n - 1] * region[m + n - 1] - region[m - 1] * region[m - 1];
            s64 k = kmin + (s64)m * COARSE;
            float c = dot(template_points, region + m, n) * inverse_root(energy * tt);
            float score = c - per_k * (float)(s32)(k > natural ? k - natural : natural - k);
            if (score > best) { best = score; best_r = c; at = k; }
        }
        /* The starts between the coarse ones around the best. */
        s64 centre = at;
        for (s64 k = centre - (s64)COARSE + 1; k < centre + (s64)COARSE; ++k) {
            if (k == centre || k < kmin || k > kmax) continue;
            gather(near, n, z, pair, nominal + k * per, coarse, s->lap);
            float c = ncc(template_points, near, n);
            float score = c - per_k * (float)(s32)(k > natural ? k - natural : natural - k);
            if (score > best) { best = score; best_r = c; at = k; }
        }
    }
    publish(l, s->serial, PLAN_FILL, at * per, law_of(best_r));
}

/* The channel's next join, once it is near enough. */
void joins_plan(u32 channel, const struct lock_table *t)
{
    struct lock *l = &timelock_locks[channel];
    u32 serial = l->serial;
    if (!l->engaged || (l->plan.key == serial && l->plan.kind != PLAN_NONE)) return;
    struct lock s;
    memcpy(&s, l, sizeof(s));
    if (l->serial != serial || !s.engaged) return; /* an event came while copying */
    const struct lock_zone *z = &t->zone[channel][s.zone];
    s32 step = patch_grain_steps[channel];
    if (!z->count || step <= 0 || !z->base) return;
    s64 per = (s64)step << (32u - AUDIO_STEP_SHIFT), base = (s64)z->base << (32u - AUDIO_STEP_SHIFT);
    s64 to_onset = s.b_hit && s.onset_at != ONSET_NONE ? (s.onset_at - s.timeline) / base : HORIZON_FRAMES + 1;
    s64 to_fill = s.mode == MODE_FILL && s.due != DUE_NONE ? (s64)s.due - s.o : HORIZON_FRAMES + 1;
    int onset = to_onset <= to_fill;
    s64 ahead = onset ? to_onset : to_fill;
    if (ahead > (s64)HORIZON_FRAMES) return;
    if (ahead < (s64)LEAD_FRAMES) { ++joins_late; return; }
    s64 now = s.expect + s.lap + ahead * per; /* where the voice will be */
    u32 pair = t->pair[channel];
    if (onset) plan_onset(l, &s, z, pair, now, per);
    else plan_fill(l, &s, z, pair, now, s.timeline + ahead * base, per);
}
