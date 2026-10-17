#include "ext.h"
#include "panel_page.h"

/* Grain mode: one channel, or a channel and its Stereo/Right partner, plays
 * overlapping windowed grains of its sample in place of its ordinary voice.
 *
 * The stock voice still runs and stays in charge. A grain starts where the
 * voice's playhead is, at the voice's pitch (keyboard, pitch CV and FM
 * included), and keeps that pitch for its life. The grain mix replaces the
 * voice's rendered sample before channel effects, so the envelope, level,
 * Bits, EQ, pan and mix all apply. Scan is how fast the start moves through
 * the zone against the voice: 100 follows the playhead, 0 holds where the
 * note began, and anything between stretches time at the voice's pitch.
 * Shape and Skew make each grain's window; on a pair the grains spread
 * across the stereo field as they grow more random. With every grain
 * sounding, a new one takes the one nearest its end and the next nearest
 * fades out ahead of the grain after.
 *
 * The settings are a preset's and live in the overlay (patch_grain). Eight
 * of the fields have a CV route; the foreground adds each route's CV to its
 * field and derives the per-frame values from the result, and builds the
 * table of sample ranges. The audio IRQ owns the grains. The foreground
 * cannot interrupt the IRQ, so a table is published by flipping one index
 * and the per-frame values are copied in with the IRQ masked. */

#define GRAINS                 8u
#define SCAN_FOLLOW            100         /* the start keeps to the playhead */
#define SCAN_STEP_SHIFT        16u
#define TURN_MARGIN            64u         /* frames before the loop's end that count as the loop coming round */
#define GRAIN_OFF              0xffu
#define OUTPUT_FRAMES_PER_MS   48u
#define TENTHS_PER_MS          10u
#define PERCENT                100u
#define CENTS_PER_OCTAVE_Q30   894785      /* 2^30 / 1200 */
#define ONE_Q30                (1u << 30)
#define ONE_Q16                (1u << 16)
#define ONE_Q15                (1 << 15)
#define INTERVAL_SHIFT         8u          /* grain interval: Q8 frames */
#define SAMPLE_SHIFT           4u          /* Q31 PCM to Q27: room for the cubic and eight grains */
#define EDGE_SHIFT             12u         /* window fade arithmetic: Q20 of the grain */
#define WHOLE_GRAIN            (1ull << 32) /* a grain's phase, end to end */
/* The window (after Mutable Instruments Beads' grain envelope, MIT): Shape
 * is the share of the grain spent fading, Skew splits it between the
 * attack and the release. At Skew 0 both fades are smooth; toward either
 * end the attack turns linear and the release a fourth-power tail, so
 * -100 is a struck decay and +100 a swell that cuts off. Each fade has a
 * floor so a short grain does not click, longer for a sharp tail. */
#define SKEW_FULL              100
#define ATTACK_MIN_FRAMES      24u         /* 0.5 ms */
#define RELEASE_MIN_FRAMES     24u         /* 0.5 ms, a smooth release */
#define RELEASE_SHARP_FRAMES   480u        /* 10 ms, a fourth-power release */
#define SMOOTH_ONE             (1 << 15)   /* Q15: all smooth fade */
/* Grains stolen to make room fade out over at most this, and by the next grain. */
#define STEAL_FADE_FRAMES      240u        /* 5 ms */
#define RATE_MIN               (1 << 10)   /* below this the voice is not moving */
#define RATE_MAX               (1 << 29)   /* 16 frames per frame */
#define SPAN_MARGIN            4u          /* frames the cubic reads around a grain */
#define DRIFT_REWRAP_LIMIT     4u          /* zone lengths of Scan drift folded by subtraction */
#define DETUNES                16u
/* Pan spread widens with the grains' randomness (after Beads): Spray
 * against the grain's length, Detune against an octave, and Octave and
 * Reverse by how evenly they split the grains; their sum, at most one,
 * times half the grains sounding is the spread. Pans gather toward the
 * middle. A panned grain keeps its near side and crosses the far side's
 * signal over at about equal power. */
#define DETUNE_SCATTER_CENTS   1200
#define FRACTION_ONE_Q16       (1u << 16)
#define RANDOM_SEED            0x2545f491u
/* The stock voice (ext.h): a silent one reads as parked; the zone it plays
 * is in the low bits of its selector word. */
#define PLAYHEAD_PARKED        0xffffffffu
#define ZONE_SELECTOR_BASE     0x9ffff580u /* ENGINE_CHANNEL_ZONE_SELECTOR_BASE */
#define ZONE_SELECTOR_STRIDE   4u
#define ZONE_SELECTOR_MASK     7u
#define CHANNEL_REVERSE        0x265u
#define SAMPLE_BUFFER_REVERSE  2u          /* buffers 2 and 3 are 0 and 1 reversed */

#define SLIDE_WHOLE            1000        /* Slide's top: the zone's length */
#define NORM_GLIDE_SHIFT       6u          /* the output gain reaches a new value over ~64 frames */
/* What a route's CV adds at +1.00 (EQ_CV_AMOUNT_MAX): five volts sweep the
 * field's span, or for Size and Overlap five octaves. Slide's span is the
 * zone's length and it goes round the zone. */
#define ROUTE_VOLTS            5
#define ROUTE_CENTS_PER_VOLT   1200
#define ROUTE_COUNT_SCALE      (EQ_CV_COUNTS_FULL * EQ_CV_AMOUNT_MAX)
/* A grain in the view: its sample frame, its window level in eighths, and
 * whether it sounds and plays backwards. */
#define VIEW_SOUNDING          0x80000000u
#define VIEW_REVERSE           0x40000000u
#define VIEW_LEVEL_SHIFT       27u
#define VIEW_FRAME_MASK        ((1u << VIEW_LEVEL_SHIFT) - 1u)
#define VIEW_BYTES             (8u + GRAINS * 4u)
const struct grain_route grain_routes[GRAIN_ROUTES] = {
    [GRAIN_ROUTE_SIZE] = {GRAIN_SIZE, 0}, [GRAIN_ROUTE_OVERLAP] = {GRAIN_OVERLAP, 0},
    [GRAIN_ROUTE_SPRAY] = {GRAIN_SPRAY, 2000}, [GRAIN_ROUTE_DETUNE] = {GRAIN_DETUNE, 1200},
    [GRAIN_ROUTE_SLIDE] = {GRAIN_SLIDE, SLIDE_WHOLE}, [GRAIN_ROUTE_SCAN] = {GRAIN_SCAN, 800},
    [GRAIN_ROUTE_SHAPE] = {GRAIN_SHAPE, 100}, [GRAIN_ROUTE_PAN] = {GRAIN_PAN, 100},
};

struct range {
    const s32 *pcm;     /* the zone's first frame, left side */
    u32 first;          /* that frame's place in the sample */
    u32 frames;         /* 0: no sample ready */
    u32 loop, turn;     /* the zone's loop: its first frame and the frame it comes round at */
    s32 right;          /* words from the left buffer to the right; 0 mono */
};
struct ranges { struct range zone[ZONES]; u32 pair; };

struct fades { u32 attack, attack_inverse, release, release_inverse, smooth; };
struct grain {
    const s32 *pcm;
    s32 right;
    u32 first;          /* pcm's frame in the sample */
    s64 position;       /* Q32 frames from pcm */
    s32 step;           /* Q25 frames per output frame; negative backwards */
    u32 phase, phase_step; /* Q32 across the grain; a zero step is a free grain */
    struct fades fades; /* its window, as it was when it started */
    u32 fade, fade_step; /* Q30 level while stolen; a zero step is not stolen */
    s32 gain[2][2];     /* Q15, [out][in]: left and right from left and right */
};

/* The fields the engine plays: the settings with each route's CV added. */
static s32 values[GRAIN_FIELDS];
static u32 grain_channel, revision, loaded;
/* One change asked for by the panel interrupt, applied by the foreground. */
static struct { volatile u32 waiting; u32 channel, field; s32 value; } request;
static struct ranges tables[2];
static volatile u32 live;
static struct tuning {
    u32 frames, phase_step, interval, norm, spray, octave, reverse;
    struct fades fades;
    u32 scatter;        /* Q16, at most one: the share of the grains' randomness */
    s32 pan, scan, backwards;
    s32 slide; /* Q16 share of the zone, either way */
    u32 detune[DETUNES]; /* Q16 pitch ratios across the spread */
} tuning;
/* IRQ-owned. */
static struct grain grains[GRAINS];
static s32 countdown, norm;
static s64 drift;
static u32 heard; /* the playhead's place in its zone a frame ago */
static u32 seed, spawned, skipped, stolen;

/* 2^(cents/1200) as Q16, within an octave either way. */
u32 grain_ratio_q16(s32 cents)
{
    static const u32 c[] = {10322906u, 59597033u, 257940273u, 744261118u}; /* ln2^k/k!, Q30 */
    s32 x = cents * CENTS_PER_OCTAVE_Q30, whole = x >> 30;
    u32 f = (u32)(x - whole * (s32)ONE_Q30);
    u64 y = c[0];
    for (u32 i = 1; i < 4; ++i) y = c[i] + ((y * f) >> 30);
    u32 mantissa = ONE_Q30 + (u32)((y * f) >> 30);
    return mantissa >> (30 - 16 - whole);
}

/* The whole square root. */
static u32 root(u32 x)
{
    u32 r = 0;
    for (u32 bit = 1u << 15; bit; bit >>= 1) if ((r | bit) * (r | bit) <= x) r |= bit;
    return r;
}

/* The reciprocal that turns a phase within a fade into Q30 of it. */
static u32 inverse(u32 reach)
{
    u32 r = reach >> EDGE_SHIFT;
    return ONE_Q30 / (r ? r : 1u);
}

/* The window's fades as phases: Shape's share of the grain, split by Skew,
 * each at least its floor; a grain shorter than its floors is all fade. */
static struct fades fades_for(u32 phase_step, u32 shape, s32 skew)
{
    struct fades f;
    u32 sharp = (u32)(skew < 0 ? -skew : skew);
    u64 fading = WHOLE_GRAIN * shape / PERCENT;
    u64 attack = fading * (u32)(SKEW_FULL + skew) / (2 * SKEW_FULL), release = fading - attack;
    u64 attack_min = (u64)ATTACK_MIN_FRAMES * phase_step;
    u64 release_min = (u64)(RELEASE_MIN_FRAMES + (RELEASE_SHARP_FRAMES - RELEASE_MIN_FRAMES) * sharp / SKEW_FULL) * phase_step;
    if (attack < attack_min) attack = attack_min;
    if (release < release_min) release = release_min;
    if (attack + release >= WHOLE_GRAIN) {
        /* Each fade keeps its smooth floor, or half a grain shorter than two. */
        u64 sum = attack + release, half = WHOLE_GRAIN / 2u;
        u64 attack_keep = attack_min < half ? attack_min : half;
        u64 release_keep = (u64)RELEASE_MIN_FRAMES * phase_step;
        if (release_keep > half) release_keep = half;
        attack = attack * (WHOLE_GRAIN - 1u) / sum;
        if (attack < attack_keep) attack = attack_keep;
        if (WHOLE_GRAIN - 1u - attack < release_keep) attack = WHOLE_GRAIN - 1u - release_keep;
        release = WHOLE_GRAIN - 1u - attack;
    }
    f.attack = (u32)attack; f.attack_inverse = inverse(f.attack);
    f.release = (u32)release; f.release_inverse = inverse(f.release);
    f.smooth = (u32)(SKEW_FULL - (s32)sharp) * SMOOTH_ONE / SKEW_FULL;
    return f;
}

/* The window's mean and mean square, Q16 of a grain at full level, for the
 * output gain: full between the fades, and each fade blends its base curve
 * with the smooth one by s. Attack t: mean 1/2, mean square 1/3 + s/30 +
 * s^2/210. Release t^4: mean 1/5 + 3s/10, mean square 1/9 + 17s/126 +
 * 79s^2/630. */
static void window_moments(const struct fades *f, u32 *mean, u32 *square)
{
    u32 a = f->attack >> 16, r = f->release >> 16, s = f->smooth << 1, s2 = (u32)(((u64)s * s) >> 16);
    u32 flat = FRACTION_ONE_Q16 - a - r;
    u32 release_mean = FRACTION_ONE_Q16 / 5u + 3u * s / 10u;
    u32 attack_square = FRACTION_ONE_Q16 / 3u + s / 30u + s2 / 210u;
    u32 release_square = FRACTION_ONE_Q16 / 9u + 17u * s / 126u + 79u * s2 / 630u;
    *mean = flat + a / 2u + (u32)(((u64)r * release_mean) >> 16);
    *square = flat + (u32)(((u64)a * attack_square) >> 16) + (u32)(((u64)r * release_square) >> 16);
}

/* How even a percent's split of the grains is, Q16: 4p(1 - p). */
static u32 split(u32 percent) { return 4u * percent * (PERCENT - percent) * FRACTION_ONE_Q16 / (PERCENT * PERCENT); }

/* The per-frame values of the fields, handed to the IRQ whole. */
static void derive(void)
{
    struct tuning next;
    u32 frames = (u32)values[GRAIN_SIZE] * OUTPUT_FRAMES_PER_MS / TENTHS_PER_MS;
    u32 overlap = (u32)values[GRAIN_OVERLAP];
    next.frames = frames;
    next.phase_step = ~0u / frames;
    next.interval = (frames << INTERVAL_SHIFT) * PERCENT / overlap;
    next.fades = fades_for(next.phase_step, (u32)values[GRAIN_SHAPE], values[GRAIN_SKEW]);
    /* Grains that line up add in amplitude: overlap x the window's mean.
     * Grains that read different sound add in power: overlap x its mean
     * square. Each is brought back to the voice's level. A pair's pans keep
     * about equal power, and grains that line up are never panned. */
    u32 mean, square;
    window_moments(&next.fades, &mean, &square);
    u32 cover = overlap * mean / PERCENT, power = overlap * square / PERCENT;
    int scattered = values[GRAIN_SPRAY] || values[GRAIN_DETUNE] || values[GRAIN_OCTAVE] ||
                    values[GRAIN_REVERSE] || values[GRAIN_SCAN] != SCAN_FOLLOW;
    if (scattered) next.norm = power <= FRACTION_ONE_Q16 ? ONE_Q16 : (1u << 24) / root(power);
    else next.norm = cover <= ONE_Q16 ? ONE_Q16 : ~0u / cover;
    next.spray = (u32)values[GRAIN_SPRAY] * OUTPUT_FRAMES_PER_MS;
    next.octave = (u32)values[GRAIN_OCTAVE] * 256u / PERCENT;
    next.reverse = (u32)values[GRAIN_REVERSE] * 256u / PERCENT;
    u64 spray_share = (u64)next.spray * FRACTION_ONE_Q16 / frames;
    u64 scatter = (spray_share < FRACTION_ONE_Q16 ? spray_share : FRACTION_ONE_Q16) +
                  (u32)values[GRAIN_DETUNE] * FRACTION_ONE_Q16 / DETUNE_SCATTER_CENTS +
                  split((u32)values[GRAIN_OCTAVE]) + split((u32)values[GRAIN_REVERSE]);
    next.scatter = scatter < FRACTION_ONE_Q16 ? (u32)scatter : FRACTION_ONE_Q16;
    next.pan = values[GRAIN_PAN] * ONE_Q15 / (s32)PERCENT;
    next.backwards = values[GRAIN_SCAN] < 0;
    next.slide = values[GRAIN_SLIDE] * (s32)ONE_Q16 / SLIDE_WHOLE;
    next.scan = (values[GRAIN_SCAN] - SCAN_FOLLOW) * (1 << SCAN_STEP_SHIFT) / (s32)PERCENT;
    for (u32 i = 0; i < DETUNES; ++i)
        next.detune[i] = grain_ratio_q16(values[GRAIN_DETUNE] * (2 * (s32)i - (s32)(DETUNES - 1)) / (s32)(DETUNES - 1));
    u32 irq = cpu_irq_save();
    tuning = next;
    cpu_irq_restore(irq);
}

/* `value` moved by cents, any number of octaves. */
static s32 scaled(s32 value, s32 cents)
{
    s32 octaves = cents / ROUTE_CENTS_PER_VOLT, rest = cents % ROUTE_CENTS_PER_VOLT;
    s64 wide = ((s64)value * grain_ratio_q16(rest)) >> 16;
    return (s32)(octaves < 0 ? wide >> -octaves : wide << octaves);
}

/* The fields with their routes' CV added; derive again when one moved. */
static void modulate(void)
{
    const struct grain_settings *g = &patch_grain;
    s32 next[GRAIN_FIELDS];
    for (u32 f = 0; f < GRAIN_FIELDS; ++f) next[f] = g->value[f];
    for (u32 r = 0; r < GRAIN_ROUTES; ++r) {
        u32 f = grain_routes[r].field;
        s32 low = grain_ranges[f].low, high = grain_ranges[f].high;
        s64 turn = (s64)eq_cv_counts(g->source[r], grain_channel & (PRESET_CHANNELS - 1u)) * g->amount[r] * ROUTE_VOLTS;
        if (!turn) continue;
        if (!grain_routes[r].span) next[f] = scaled(next[f], (s32)(turn * ROUTE_CENTS_PER_VOLT / ROUTE_COUNT_SCALE));
        else next[f] += (s32)(turn * grain_routes[r].span / (ROUTE_COUNT_SCALE * ROUTE_VOLTS));
        if (f == GRAIN_SLIDE) next[f] %= SLIDE_WHOLE;
        next[f] = next[f] < low ? low : next[f] > high ? high : next[f];
    }
    if (!memcmp(next, values, sizeof(values))) return;
    memcpy(values, next, sizeof(values));
    derive();
}

/* The voice's frame in its sample. A released voice keeps going round its
 * loop with its envelope shut, so a silent one reads as parked. */
static u32 playhead(u32 channel)
{
    if ((s32)REG32(LEVEL_WORD_BASE + channel * LEVEL_WORD_STRIDE) < LEVEL_SILENT) return PLAYHEAD_PARKED;
    return REG32(PLAYHEAD_FRAME_BASE + channel * PLAYHEAD_FRAME_STRIDE);
}

/* A start in the zone after Slide, wrapped to the zone. `frame` is within a
 * zone's length of it either way. */
static s32 placed(s32 frame, const struct range *r)
{
    s32 length = (s32)r->frames;
    frame += (s32)(((s64)length * tuning.slide) >> 16);
    while (frame < 0) frame += length;
    while (frame >= length) frame -= length;
    return frame;
}

static u32 random(void)
{
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    return seed;
}

/* A pan's gains for a pair, Q15 [out][in]: the near side keeps its own
 * signal and takes the far side's at 1 - (1 - |d|)^2; the far side keeps
 * 1 - d^2 of its own. */
static void pan_gains(s32 gain[2][2], s32 d)
{
    s32 m = d < 0 ? -d : d, far = ONE_Q15 - (s32)(((s64)m * m) >> 15), cross = ONE_Q15 - (s32)(((s64)(ONE_Q15 - m) * (ONE_Q15 - m)) >> 15);
    u32 near = d < 0 ? 0u : 1u;
    gain[near][near] = ONE_Q15; gain[near][near ^ 1u] = d ? cross : 0;
    gain[near ^ 1u][near ^ 1u] = d ? far : ONE_Q15; gain[near ^ 1u][near] = 0;
}

/* `due` is how long ago the grain should have started, Q8 frames. */
static void spawn(struct grain *g, u32 channel, s32 step, u32 sounding, u32 due)
{
    const struct range *r = &tables[live].zone[REG32(ZONE_SELECTOR_BASE + channel * ZONE_SELECTOR_STRIDE) & ZONE_SELECTOR_MASK];
    u32 at = playhead(channel) - r->first;
    u32 chance = random();
    s64 wide = ((s64)(step < 0 ? -step : step) * (s32)tuning.detune[chance % DETUNES]) >> 16;
    if (((chance >> 8) & 0xffu) < tuning.octave) wide *= 2;
    s32 rate = wide > RATE_MAX ? RATE_MAX : (s32)wide;
    u32 span = (u32)(((u64)tuning.frames * (u32)rate) >> AUDIO_STEP_SHIFT) + SPAN_MARGIN;
    /* A voice that is not playing parks its playhead past the sample. */
    if (at >= r->frames || rate < RATE_MIN || span >= r->frames) { ++skipped; return; }

    /* Scan drift moves the start around the zone; Spray scatters it. */
    s32 length = (s32)r->frames, moved = (s32)(drift >> 32);
    if (moved >= length || moved <= -length) {
        if (moved >= (s32)DRIFT_REWRAP_LIMIT * length || moved <= -(s32)DRIFT_REWRAP_LIMIT * length) drift = 0;
        while ((s32)(drift >> 32) >= length) drift -= (s64)length << 32;
        while ((s32)(drift >> 32) <= -length) drift += (s64)length << 32;
        moved = (s32)(drift >> 32);
    }
    s32 start = placed((s32)at + moved, r);
    if (tuning.spray) start += (s32)(((u64)random() * tuning.spray) >> 32) - (s32)(tuning.spray >> 1);
    s32 last = length - (s32)span;
    if (start < 1) start = 1;
    if (start > last) start = last;

    g->pcm = r->pcm; g->right = r->right; g->first = r->first;
    if (((chance >> 16) & 0xffu) < tuning.reverse) {
        g->position = (s64)(start + (s32)(span - SPAN_MARGIN)) << 32;
        g->step = -rate;
    } else {
        g->position = (s64)start << 32;
        g->step = rate;
    }
    /* The spread is the grains' randomness times half of them sounding,
     * this one counted; a second random draw gathers pans to the middle. */
    u32 pick = random(), spread = (sounding + 1u) * tuning.scatter / 2u;
    u32 reach = (u32)(((u64)spread * (pick >> 16)) >> 16);
    if (reach > FRACTION_ONE_Q16) reach = FRACTION_ONE_Q16;
    s32 d = (s32)((((s64)((s32)(pick & 0xffffu) - ONE_Q15) * (s32)reach) >> 16) * tuning.pan >> 15);
    pan_gains(g->gain, d);
    g->position += ((s64)g->step << (32u - AUDIO_STEP_SHIFT)) * due >> INTERVAL_SHIFT;
    g->phase_step = tuning.phase_step;
    g->phase = (g->phase_step >> INTERVAL_SHIFT) * due;
    g->fades = tuning.fades;
    g->fade = ONE_Q30; g->fade_step = 0;
    ++spawned;
}

static inline s32 mul31(s32 x, u32 f) { return (s32)(((s64)x * (s32)f) >> 31); }

/* Four-point cubic through p[-1..2] at fraction f (Q31 of a frame). */
static inline s32 interpolate(const s32 *p, u32 f)
{
    s32 a = p[-1] >> SAMPLE_SHIFT, b = p[0] >> SAMPLE_SHIFT, c = p[1] >> SAMPLE_SHIFT, d = p[2] >> SAMPLE_SHIFT;
    s32 c1 = (c - a) >> 1;
    s32 c2 = a - 2 * b - (b >> 1) + 2 * c - (d >> 1);
    s32 c3 = ((d - a) >> 1) + (b - c) + ((b - c) >> 1);
    return mul31(mul31(mul31(c3, f) + c2, f) + c1, f) + b;
}

/* The grain's window at its phase, Q30: full between its fades. The attack
 * blends a line, the release a fourth power, with the smooth fade. */
static inline u32 window(const struct grain *g)
{
    const struct fades *f = &g->fades;
    u32 phase = g->phase, to_end = ~phase, x, base;
    if (phase < f->attack) base = x = (phase >> EDGE_SHIFT) * f->attack_inverse;
    else if (to_end < f->release) {
        x = (to_end >> EDGE_SHIFT) * f->release_inverse;
        u32 square = (u32)(((u64)x * x) >> 30);
        base = (u32)(((u64)square * square) >> 30);
    } else return g->fade;
    u32 square = (u32)(((u64)x * x) >> 30);
    u32 smooth = (u32)(((u64)square * (3u * ONE_Q30 - 2u * x)) >> 30);
    u32 w = base + (u32)((((s64)smooth - (s64)base) * (s32)f->smooth) >> 15);
    return g->fade_step ? (u32)(((u64)w * g->fade) >> 30) : w;
}

/* The Q27 grain sum at unity, as a saturated Q31 sample. */
static s32 output(s64 sum)
{
    s64 wide = (sum * norm) >> (16u - SAMPLE_SHIFT);
    return wide > 0x7fffffff ? 0x7fffffff : wide < -0x7fffffff - 1 ? -0x7fffffff - 1 : (s32)wide;
}

static void render(s32 *mix, const s32 *steps)
{
    u32 channel = grain_channel;
    /* Scan: the start falls behind the playhead, or runs ahead of it, by a
     * share of the voice's step, and goes round the zone's loop as the voice
     * does. A note starts it again from the playhead. */
    const struct range *r = &tables[live].zone[REG32(ZONE_SELECTOR_BASE + channel * ZONE_SELECTOR_STRIDE) & ZONE_SELECTOR_MASK];
    u32 at = playhead(channel) - r->first;
    s32 step = steps[channel];
    if (at < r->frames) {
        s64 round = (s64)(r->turn - r->loop) << 32;
        u32 reach = (u32)((step < 0 ? -step : step) >> AUDIO_STEP_SHIFT) + TURN_MARGIN;
        if (heard >= r->frames || (at < heard && heard + reach < r->turn)) drift = 0;
        else {
            if (at < heard) drift += round;
            drift += ((s64)step * tuning.scan) >> (SCAN_STEP_SHIFT + AUDIO_STEP_SHIFT - 32u);
            s32 place = (s32)at + (s32)(drift >> 32);
            if (place >= (s32)r->turn) drift -= round;
            else if (tuning.backwards && place < (s32)r->loop) drift += round;
        }
    }
    heard = at;
    countdown -= 1 << INTERVAL_SHIFT;
    if (countdown <= 0) {
        /* A free grain, or else the one nearest its end; then, with none
         * left free, the next nearest fades out in time for the next grain. */
        struct grain *taken = 0, *oldest = 0;
        u32 sounding = 0;
        for (u32 n = 0; n < GRAINS; ++n) {
            struct grain *g = &grains[n];
            if (!g->phase_step) { if (!taken) taken = g; continue; }
            ++sounding;
            if (!oldest || g->phase > oldest->phase) oldest = g;
        }
        if (!taken) { taken = oldest; --sounding; ++stolen; }
        u32 due = (u32)-countdown;
        countdown += (s32)tuning.interval;
        spawn(taken, channel, steps[channel], sounding, due);
        if (taken->phase_step && sounding + 1u >= GRAINS) {
            struct grain *next_out = 0;
            for (u32 n = 0; n < GRAINS; ++n) {
                struct grain *g = &grains[n];
                if (g != taken && g->phase_step && !g->fade_step && (!next_out || g->phase > next_out->phase)) next_out = g;
            }
            u32 fade = tuning.interval >> INTERVAL_SHIFT;
            if (fade > STEAL_FADE_FRAMES) fade = STEAL_FADE_FRAMES;
            if (next_out && ~next_out->phase / next_out->phase_step > fade) {
                next_out->fade_step = ONE_Q30 / (fade ? fade : 1u);
                ++stolen;
            }
        }
    }
    u32 pair = tables[live].pair;
    s64 left = 0, right = 0;
    for (u32 n = 0; n < GRAINS; ++n) {
        struct grain *g = &grains[n];
        if (!g->phase_step) continue;
        u32 w = window(g), f = (u32)g->position >> 1;
        const s32 *p = g->pcm + (s32)(g->position >> 32);
        __builtin_prefetch(p + (g->step < 0 ? -(s32)(CACHE_LINE_BYTES / 4) : (s32)(CACHE_LINE_BYTES / 4)));
        s32 sample = (s32)(((s64)interpolate(p, f) * (s32)w) >> 30);
        if (!pair) left += sample;
        else {
            s32 other = g->right ? (s32)(((s64)interpolate(p + g->right, f) * (s32)w) >> 30) : sample;
            left += ((s64)sample * g->gain[0][0] + (s64)other * g->gain[0][1]) >> 15;
            right += ((s64)sample * g->gain[1][0] + (s64)other * g->gain[1][1]) >> 15;
        }
        g->position += (s64)g->step << (32u - AUDIO_STEP_SHIFT);
        u32 next = g->phase + g->phase_step;
        if (next < g->phase) g->phase_step = 0;
        else g->phase = next;
        if (g->fade_step) {
            if (g->fade <= g->fade_step) g->phase_step = 0;
            else g->fade -= g->fade_step;
        }
    }
    /* The output gain follows Overlap, Shape and Pan without a step. */
    norm += ((s32)tuning.norm - norm) >> NORM_GLIDE_SHIFT;
    mix[channel] = output(left);
    if (pair) mix[channel + 1] = output(right);
}

/* The channel's zones: each ready zone's part of its sample, in the buffer
 * its voice plays (the reversed copy when Reverse is set). */
static void ranges_build(struct ranges *out, u32 channel)
{
    memset(out, 0, sizeof(*out));
    const u8 *record = channel_record(channel);
    u32 reversed = record[CHANNEL_REVERSE] ? SAMPLE_BUFFER_REVERSE : 0;
    out->pair = channel + 1 < PRESET_CHANNELS && channel_record(channel + 1)[CHANNEL_MODE] == CHANNEL_MODE_STEREO_RIGHT;
    for (u32 z = 0; z < ZONES; ++z) {
        const u8 *zone = zone_record(channel, z), *sample = zone[0] ? ready_sample(channel, z) : 0;
        if (!sample) continue;
        u32 side = out->pair ? 0 : zone[ZONE_SIDE] & 1u;
        u32 left = REG32((u32)sample + SAMPLE_BUFFER_0 + (reversed + side) * 4);
        u32 right = out->pair ? REG32((u32)sample + SAMPLE_BUFFER_0 + (reversed + 1) * 4) : 0;
        u32 frames = REG32((u32)sample + SAMPLE_FRAMES);
        u32 start = get_u32(zone + ZONE_SAMPLE_START), end = get_u32(zone + ZONE_SAMPLE_END);
        if (end > frames) end = frames;
        if (!left || start >= end) continue;
        struct range *r = &out->zone[z];
        r->pcm = (const s32 *)left + SAMPLE_GUARD_FRAMES + start;
        r->first = start;
        r->frames = end - start;
        u32 loop = get_u32(zone + ZONE_LOOP_START), length = get_u32(zone + ZONE_LOOP_LENGTH) >> ZONE_LOOP_LENGTH_SHIFT;
        if (loop >= start && loop < end && length && length < end - loop) { r->loop = loop - start; r->turn = r->loop + length; }
        else r->turn = r->frames;
        r->right = right ? (s32)(right - left) / 4 : 0;
    }
}

/* The audio interrupt's frame: grain mode's channel, then a channel armed
 * for an audition. */
static void frame(s32 *mix, const s32 *steps)
{
    if (grain_channel != GRAIN_OFF) render(mix, steps);
    audition_frame(mix, steps);
}

/* The hook runs only while grain mode or an audition needs it; the IRQ
 * reads the pointer once per frame. */
void audio_hook_update(void)
{
    MEMORY_BARRIER();
    patch_grain_hook = grain_channel != GRAIN_OFF || audition_armed() ? frame : 0;
}

static void stop(void)
{
    grain_channel = GRAIN_OFF; /* the frame checks the channel before it renders */
    audio_hook_update();
}

static void start(u32 channel)
{
    stop();
    memset(grains, 0, sizeof(grains));
    countdown = 0; drift = 0; seed = RANDOM_SEED;
    ranges_build(&tables[0], channel);
    live = 0;
    derive();
    norm = (s32)tuning.norm;
    MEMORY_BARRIER();
    grain_channel = channel;
    audio_hook_update();
}

static s32 clamp(s32 value, s32 low, s32 high) { return value < low ? low : value > high ? high : value; }

/* One setting, clamped to its range: a field, or past GRAIN_REQUEST_ROUTE a
 * route's source or amount. Grain On picks the channel, taking it from any
 * other; the rest belong to the one engine. */
static u32 store(u32 channel, u32 field, s32 value)
{
    struct grain_settings *g = &patch_grain;
    if (field >= GRAIN_REQUEST_ROUTE) {
        u32 route = (field - GRAIN_REQUEST_ROUTE) / 2;
        if (route >= GRAIN_ROUTES) return BAD_RANGE;
        if (field & 1u) g->amount[route] = (signed char)clamp(value, -EQ_CV_AMOUNT_MAX, EQ_CV_AMOUNT_MAX);
        else g->source[route] = (signed char)clamp(value, EQ_CV_SOURCE_OFF, EQ_CV_SOURCE_MAX);
    } else if (field == GRAIN_ON) {
        if (value && channel_record(channel)[CHANNEL_MODE] == CHANNEL_MODE_STEREO_RIGHT) return BAD_STATE;
        if (value) start(channel);
        else if (grain_channel == channel) stop();
        g->value[GRAIN_ON] = grain_channel == GRAIN_OFF ? 0 : (s16)(grain_channel + 1);
    } else g->value[field] = (s16)clamp(value, grain_ranges[field].low, grain_ranges[field].high);
    modulate();
    ++revision;
    return OK;
}

/* A setting as it is stored: a field, or past GRAIN_REQUEST_ROUTE a route's
 * source or amount. Grain is On for the channel that plays grains. */
s32 grain_value(u32 channel, u32 field)
{
    const struct grain_settings *g = &patch_grain;
    if (field >= GRAIN_REQUEST_ROUTE) {
        u32 route = (field - GRAIN_REQUEST_ROUTE) / 2;
        return field & 1u ? g->amount[route] : g->source[route];
    }
    return field == GRAIN_ON ? grain_channel == channel : g->value[field];
}
s32 grain_low(u32 field) { return field == GRAIN_ON ? 0 : grain_ranges[field].low; }
s32 grain_high(u32 field) { return field == GRAIN_ON ? 1 : grain_ranges[field].high; }
u32 grain_revision(void) { return revision; }
/* What a remote editor mirrors: the channel, the fields and the routes. */
u32 grain_crc(void) { return ~crc32_update(~0u, &patch_grain, sizeof(patch_grain)); }

/* A preset's settings replaced ours: play its channel, if it can. */
static void adopt(void)
{
    loaded = patch_grain_loaded;
    u32 channel = (u32)patch_grain.value[GRAIN_ON] - 1u;
    stop();
    modulate(); /* the fields first: starting derives from them */
    if (channel < PRESET_CHANNELS && channel_record(channel)[CHANNEL_MODE] != CHANNEL_MODE_STEREO_RIGHT) start(channel);
    else patch_grain.value[GRAIN_ON] = 0;
    ++revision;
}

void grain_request(u32 channel, u32 field, s32 value)
{
    request.waiting = 0;
    request.channel = channel; request.field = field; request.value = value;
    MEMORY_BARRIER();
    request.waiting = 1;
}

/* Every foreground pass: take a loaded preset's settings and the panel's
 * change, then follow CV and zone, sample and Reverse changes. */
void grains_foreground(void)
{
    if (loaded != patch_grain_loaded) adopt();
    if (request.waiting) {
        u32 irq = cpu_irq_save();
        u32 channel = request.channel, field = request.field;
        s32 value = request.value;
        request.waiting = 0;
        cpu_irq_restore(irq);
        store(channel, field, value);
    }
    if (grain_channel == GRAIN_OFF) return;
    if (channel_record(grain_channel)[CHANNEL_MODE] == CHANNEL_MODE_STEREO_RIGHT) { store(grain_channel, GRAIN_ON, 0); return; }
    modulate();
    u32 next = live ^ 1u;
    ranges_build(&tables[next], grain_channel);
    if (!memcmp(&tables[next], &tables[live], sizeof(tables[0]))) return;
    MEMORY_BARRIER();
    live = next;
    derive();
}

/* Set one field and reply the stored value, or with GRAIN_SET_STATUS reply the
 * engine's state:
 *   u8 channel (FFh off), pair, sounding grains, the voice's zone;
 *   u32 grains started, skipped (the voice not playing, or the grain
 *   longer than its zone) and stolen (grains cut short to make room);
 *   s32 pitch step, u32 playhead frame, u32 the zone's first frame and
 *   frames; then the fields as s16, Size first and Skew last.
 * GRAIN_SET_VIEW replies what is sounding, for a display: u8 channel (FFh off),
 * u8 the voice's zone, u16 zero, u32 playhead frame, then each grain as a
 * u32: its sample frame, window level in eighths from bit 27, bit 30
 * backwards, bit 31 sounding.
 * GRAIN_SET_ROUTE and the fifteen after it set a route's source, then its
 * amount; GRAIN_SET_ROUTES replies every route's source and amount as s8.
 * GRAIN_SET_SKEW sets Skew. */
u32 grain_set(u32 channel, u32 field, s32 value, u8 *out, u32 *count)
{
    if (channel >= PRESET_CHANNELS || field > GRAIN_SET_SKEW ||
        (field > GRAIN_SET_VIEW && field < GRAIN_SET_SLIDE)) return BAD_RANGE;
    if (field == GRAIN_SET_ROUTES) {
        for (u32 r = 0; r < GRAIN_ROUTES; ++r) { out[2*r] = (u8)patch_grain.source[r]; out[2*r + 1] = (u8)patch_grain.amount[r]; }
        *count = 2 * GRAIN_ROUTES;
        return OK;
    }
    if (field == GRAIN_SET_VIEW) {
        u32 on = grain_channel != GRAIN_OFF, at = on ? grain_channel : channel;
        memset(out, 0, VIEW_BYTES);
        out[0] = (u8)grain_channel;
        out[1] = (u8)(REG32(ZONE_SELECTOR_BASE + at * ZONE_SELECTOR_STRIDE) & ZONE_SELECTOR_MASK);
        /* One frame's grains: the audio interrupt owns them. The playhead
         * sent is where grains start, which Scan moves apart from the voice. */
        u32 irq = cpu_irq_save();
        u32 head = playhead(at);
        const struct range *r = &tables[live].zone[out[1]];
        if (on && head - r->first < r->frames) {
            head = r->first + (u32)placed((s32)(head - r->first) + (s32)(drift >> 32), r);
        }
        put_u32(out + 4, head);
        for (u32 n = 0; on && n < GRAINS; ++n) {
            const struct grain *g = &grains[n];
            if (!g->phase_step) continue;
            u32 frame = (g->first + (u32)(g->position >> 32)) & VIEW_FRAME_MASK;
            put_u32(out + 8 + n * 4, VIEW_SOUNDING | (g->step < 0 ? VIEW_REVERSE : 0) |
                    (window(g) >> (30u - 3u) > 7u ? 7u : window(g) >> (30u - 3u)) << VIEW_LEVEL_SHIFT | frame);
        }
        cpu_irq_restore(irq);
        *count = VIEW_BYTES;
        return OK;
    }
    if (field == GRAIN_SET_STATUS) {
        u32 sounding = 0, on = grain_channel != GRAIN_OFF, at = on ? grain_channel : channel;
        for (u32 n = 0; n < GRAINS; ++n) sounding += grains[n].phase_step != 0;
        const struct ranges *t = &tables[live];
        u32 zone = REG32(ZONE_SELECTOR_BASE + at * ZONE_SELECTOR_STRIDE) & ZONE_SELECTOR_MASK;
        out[0] = (u8)grain_channel; out[1] = (u8)(on && t->pair); out[2] = (u8)sounding; out[3] = (u8)zone;
        put_u32(out + 4, spawned); put_u32(out + 8, skipped); put_u32(out + 12, stolen);
        put_u32(out + 16, (u32)patch_grain_steps[at]);
        put_u32(out + 20, REG32(PLAYHEAD_FRAME_BASE + at * PLAYHEAD_FRAME_STRIDE));
        put_u32(out + 24, on ? t->zone[zone].first : 0); put_u32(out + 28, on ? t->zone[zone].frames : 0);
        for (u32 n = GRAIN_SIZE; n < GRAIN_FIELDS; ++n) put_u16(out + 32 + (n - GRAIN_SIZE) * 2, (u32)patch_grain.value[n]);
        *count = 32 + (GRAIN_FIELDS - GRAIN_SIZE) * 2;
        return OK;
    }
    if (field == GRAIN_SET_SLIDE) field = GRAIN_SLIDE;
    else if (field == GRAIN_SET_SKEW) field = GRAIN_SKEW;
    else if (field >= GRAIN_SET_ROUTE) field += GRAIN_REQUEST_ROUTE - GRAIN_SET_ROUTE;
    u32 status = store(channel, field, value);
    if (status != OK) return status;
    put_u32(out, (u32)grain_value(channel, field));
    *count = 4;
    return OK;
}

void grains_init(void)
{
    spawned = skipped = stolen = 0;
    request.waiting = 0;
    adopt();
    patch_panel_page = &grain_page;
}
