#include "ext.h"

/* Time Lock: moves the stock voice's playhead to each hit on the tempo's
 * timeline and fills pitched-up slices. Positions are Q32 sample frames. */

#include "timelock.h"
#include "mix_fx.h"

struct lock_table timelock_tables[2];
volatile u32 timelock_live;
struct lock timelock_locks[PRESET_CHANNELS];
static u32 sine[SINE_STEPS + 1];     /* Q30 sin(pi/2 i/SINE_STEPS) */
static u32 reciprocal[FADES_MAX + 1]; /* Q16 / n: a fade's phase step */
static u32 seed = RANDOM_SEED;
struct timelock_stats { u32 joins, skipped, onsets, fills, runouts, resyncs, planned, waits; } timelock_stats;
struct clock timelock_clocks[CLOCKS];
static u32 events; /* left this frame: at most EVENTS_PER_FRAME, the rest a frame later */
static u32 streams_live; /* reads fading out across the channels, counted each frame */
static u32 frame_start; /* cycle count at the frame's start */

/* ---- interrupt ---- */

static inline u64 playhead_get(u32 c)
{
    u32 at = PLAYHEAD_VECTOR + c * PLAYHEAD_BYTES;
    return ((u64)REG32(at + 4u) << 32) | REG32(at);
}

static inline void playhead_set(u32 c, u64 value)
{
    u32 at = PLAYHEAD_VECTOR + c * PLAYHEAD_BYTES;
    REG32(at) = (u32)value;
    REG32(at + 4u) = (u32)(value >> 32);
}

/* The high word of a signed product: one smmul. */
static inline s32 high(s32 a, s32 b) { return (s32)(((s64)a * b) >> 32); }

/* Four-point cubic through p[-1..2] at fraction f (Q31), Q27 out, as grain
 * mode reads. */
static inline s32 interpolate(const s32 *p, s32 f)
{
    s32 a = p[-1] >> SAMPLE_SHIFT, b = p[0] >> SAMPLE_SHIFT, c = p[1] >> SAMPLE_SHIFT, d = p[2] >> SAMPLE_SHIFT;
    s32 c1 = (c - a) >> 1;
    s32 c2 = a - 2 * b - (b >> 1) + 2 * c - (d >> 1);
    s32 c3 = ((d - a) >> 1) + (b - c) + ((b - c) >> 1);
    s32 y = high(c3, f) << 1;
    y = high(y + c2, f) << 1;
    y = high(y + c1, f) << 1;
    return y + b;
}

static inline u32 sine_at(u32 phase) /* phase Q16 of a quarter turn */
{
    u32 i = phase >> 8, f = phase & 0xffu;
    if (i >= SINE_STEPS) return sine[SINE_STEPS];
    return sine[i] + (u32)(((u64)(sine[i + 1] - sine[i]) * f) >> 8);
}

/* A crossfade's two gains at its phase, Q30: equal gain (sin^2) blended
 * with equal power (sin) by the law. */
static inline u32 fade_in_gain(const struct fade *f)
{
    u32 s = sine_at(f->phase), s2 = (u32)(((u64)s * s) >> 30);
    return (u32)(((u64)s2 * f->law + (u64)s * (256u - f->law)) >> 8);
}
static inline u32 fade_out_gain(const struct fade *f)
{
    u32 c = sine_at((1u << 16) - f->phase), c2 = (u32)(((u64)c * c) >> 30);
    return (u32)(((u64)c2 * f->law + (u64)c * (256u - f->law)) >> 8);
}

static inline void fade_start(struct fade *f, u32 frames, u32 law)
{
    if (frames > FADES_MAX) frames = FADES_MAX;
    if (!frames) frames = 1;
    f->step = reciprocal[frames]; f->phase = f->step >> 1; f->law = (u8)law;
}
static inline int fade_done(const struct fade *f) { return f->phase >= (1u << 16); }

static s32 clamp(s32 x, s32 low, s32 high) { return x < low ? low : x > high ? high : x; }
static inline s32 saturate(s64 x) { return x > Q31_MAX ? Q31_MAX : x < Q31_MIN ? Q31_MIN : (s32)x; }
/* A Q27 read times a Q30 gain, as Q31. */
static inline s32 heard(s32 y, u32 g) { return high(y, (s32)(g >= ONE_Q30 ? Q31_MAX : g << 1)) << (1u + SAMPLE_SHIFT); }
/* x (Q31) times a Q30 gain. */
static inline s32 scale30(s32 x, u32 g) { return high(x, (s32)(g >= ONE_Q30 ? Q31_MAX : g << 1)) << 1; }

/* The first position index past `frame`. */
u32 timelock_after(const struct lock_zone *z, u32 frame)
{
    u32 lo = 0, hi = z->count;
    while (lo < hi) {
        u32 mid = (lo + hi) >> 1;
        if ((z->positions[mid] & ~HIT_FLAG) <= frame) lo = mid + 1; else hi = mid;
    }
    return lo;
}

static inline int looping(const struct lock_zone *z) { return z->turn < z->end; }
static inline s64 frames_q32(u32 frame) { return (s64)frame << 32; }
static inline s64 round_q32(const struct lock_zone *z) { return (s64)(z->turn - z->loop) << 32; }

/* Where the slice ends: the next hit past the cursor, going round the loop
 * once; with none, the zone's end. */
static void slice_end(struct lock *l, const struct lock_zone *z)
{
    s64 lap = l->a_lap;
    u32 i = l->cursor;
    for (u32 turns = 0; turns < 2; ++turns) {
        for (; i < z->count; ++i) {
            u32 p = z->positions[i], frame = p & ~HIT_FLAG;
            if (frame >= z->turn) break;
            if (p & HIT_FLAG) { l->b = frames_q32(frame) + lap; l->b_lap = lap; l->b_index = i; l->b_hit = 1; return; }
        }
        if (!looping(z)) break;
        i = z->loop_index; lap += round_q32(z);
    }
    l->b = frames_q32(z->end) + l->a_lap; l->b_lap = l->a_lap; l->b_index = z->count; l->b_hit = 0;
}

/* The slice holding the voice's real frame: its start is the last hit at or
 * before it, a lap back round the loop if need be. */
COLD static void slice_start(struct lock *l, const struct lock_zone *z, u32 frame)
{
    u32 floor = frame >= z->loop && looping(z) ? z->loop : z->first, i = timelock_after(z, frame);
    for (u32 k = i; k-- > 0;) {
        u32 p = z->positions[k];
        if ((p & ~HIT_FLAG) < floor) break;
        if (p & HIT_FLAG) { l->a = frames_q32(p & ~HIT_FLAG); l->a_lap = 0; l->cursor = k + 1; return; }
    }
    if (looping(z) && frame >= z->loop)
        for (u32 k = z->count; k-- > z->loop_index;) {
            u32 p = z->positions[k];
            if ((p & ~HIT_FLAG) >= z->turn) continue;
            if (p & HIT_FLAG) { l->a_lap = -round_q32(z); l->a = frames_q32(p & ~HIT_FLAG) + l->a_lap; l->cursor = k + 1; return; }
        }
    l->a = frames_q32(floor); l->a_lap = 0; l->cursor = i;
}

/* The last rise after the slice's start and before `at` (unrolled), or 0. */
static s64 last_rise(const struct lock *l, const struct lock_zone *z, s64 at)
{
    s64 found = 0;
    for (u32 i = l->cursor; i < z->count && i < l->b_index; ++i) {
        u32 frame = z->positions[i] & ~HIT_FLAG;
        if (frame >= z->turn || frames_q32(frame) + l->a_lap >= at) break;
        found = frames_q32(frame) + l->a_lap;
    }
    return found;
}

static inline u32 output_frames(const struct lock_zone *z, s64 frames_q32_) { return (u32)(((u64)(frames_q32_ >> 32) * z->inverse) >> 31); }

static u32 random(void)
{
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    return seed;
}

/* A fill piece's length: within a quarter of `piece` either way. */
static u32 jittered(u32 piece)
{
    u32 jitter = (u32)(((u64)random() * (piece >> (PIECE_JITTER_SHIFT - 1))) >> 32);
    return piece - (piece >> PIECE_JITTER_SHIFT) + jitter;
}

/* A slice starts: decide how it plays at this pitch. */
COLD static void slice_plan(struct lock *l, const struct lock_zone *z, u32 own_q16)
{
    l->length = output_frames(z, l->b - l->a);
    l->own = own_q16 ? (u32)(((u64)l->length * own_q16) >> 16) : ~0u;
    l->runout_phase = 0;
    l->due = DUE_NONE;
    l->onset_at = l->b_hit ? l->b - (s64)PREROLL_FRAMES * ((s64)z->base << STEP_TO_Q32) : l->b;
    if (l->own >= l->length) { l->mode = MODE_PLAY_ON; return; } /* pitched down: cut at the next hit */
    u32 attack = ATTACK_FRAMES;
    if (attack > l->length / 2) attack = l->length / 2;
    if (attack > l->own / 2) attack = l->own / 2;
    u32 settle = SETTLE_FRAMES < l->own / 3 ? SETTLE_FRAMES : l->own / 3;
    u32 avail = l->own - settle, rest = l->length - attack;
    u32 x = avail / 4 > FILL_FADE_MIN ? avail / 4 : FILL_FADE_MIN;
    if (x > FILL_FADE_FRAMES) x = FILL_FADE_FRAMES;
    if (x > rest / 3) x = rest / 3;
    if (x < FILL_FADE_FLOOR || avail < 3 * x || rest < 2 * x) {
        l->mode = MODE_RUNOUT;
        l->due = l->own > RUNOUT_FRAMES ? (s32)(l->own - RUNOUT_FRAMES) : 0; /* checked by position from here */
        return;
    }
    u32 piece = PIECE_FRAMES > 2 * x ? PIECE_FRAMES : 2 * x;
    if (piece > avail - x) piece = avail - x;
    l->mode = MODE_FILL;
    l->attack = attack; l->xfade = x; l->piece = piece;
    l->next_piece = jittered(piece);
    l->due = (s32)attack;
}


/* Move the voice (and its Stereo/Right partner) to `to` (unrolled) and fade
 * the read it leaves out over `frames`; the voice fades in. */
COLD static void join(struct lock *l, u32 c, u32 pair, const struct lock_zone *z, s64 now, s64 to, u32 frames, u32 law, int heard)
{
    s64 gap = to - (now + l->lap);
    if (gap < (s64)SKIP_Q32 && gap > -(s64)SKIP_Q32) { ++timelock_stats.skipped; return; }
    if (heard) {
        struct stream *s = &l->streams[0];
        if (s->active) s = &l->streams[1];
        if (s->active) /* both busy: the older gives way to this one */
            s = l->streams[0].out.phase > l->streams[1].out.phase ? &l->streams[0] : &l->streams[1];
        s->position = now;
        fade_start(&s->out, frames, law);
        s->in = l->in; /* the voice's fade-in still running goes with it */
        if (!s->active) ++streams_live;
        s->active = 1;
    }
    fade_start(&l->in, frames, law);
    if (!heard) l->in.law = 0;
    l->mixing = 1;
    /* The real frame, in the voice's lap or the one either side. */
    s64 real = to - l->lap;
    if (looping(z)) {
        if (real >= frames_q32(z->turn)) { real -= round_q32(z); l->lap += round_q32(z); }
        else if (real < frames_q32(z->loop) && l->lap > 0) { real += round_q32(z); l->lap -= round_q32(z); }
    }
    s64 lowest = frames_q32(z->first + READ_MARGIN), highest = frames_q32(z->end - READ_MARGIN);
    if (real < lowest) real = lowest;
    if (real > highest) real = highest;
    playhead_set(c, (u64)real);
    if (pair) playhead_set(c + 1, (u64)real);
    l->expect = real;
    ++timelock_stats.joins;
}

/* The fading reads and the voice's gains, mixed into this frame. */
static __attribute__((noinline)) void mix_frame(struct lock *l, s32 *mix, u32 c, u32 pair, const struct lock_zone *z, s32 step)
{
    u32 g = ONE_Q30;
    if (!fade_done(&l->in)) { g = fade_in_gain(&l->in); l->in.phase += l->in.step; }
    if (l->mode == MODE_WAIT) g = 0;
    else if (l->runout_phase) {
        g = (u32)(((u64)g * sine_at((1u << 16) - l->runout_phase)) >> 30);
        l->runout_phase += reciprocal[RUNOUT_FRAMES];
        if (l->runout_phase >= (1u << 16)) { l->mode = MODE_WAIT; l->due = 0; }
    }
    s64 left = scale30(mix[c], g), right = pair ? scale30(mix[c + 1], g) : 0;
    for (u32 n = 0; n < STREAMS; ++n) {
        struct stream *s = &l->streams[n];
        if (!s->active) continue;
        s->position += (s64)step << STEP_TO_Q32;
        if (looping(z) && s->position >= frames_q32(z->turn)) s->position -= round_q32(z);
        u32 frame = (u32)(s->position >> 32);
        if (frame < z->first + READ_MARGIN || frame + READ_MARGIN >= z->end) { s->active = 0; continue; }
        u32 w = fade_out_gain(&s->out);
        if (!fade_done(&s->in)) { w = (u32)(((u64)w * fade_in_gain(&s->in)) >> 30); s->in.phase += s->in.step; }
        s32 f = (s32)((u32)s->position >> 1);
        __builtin_prefetch(z->pcm[0] + frame + CACHE_LINE_BYTES / sizeof(s32));
        left += heard(interpolate(z->pcm[0] + frame, f), w);
        if (pair && z->pcm[1]) {
            __builtin_prefetch(z->pcm[1] + frame + CACHE_LINE_BYTES / sizeof(s32));
            right += heard(interpolate(z->pcm[1] + frame, f), w);
        }
        s->out.phase += s->out.step;
        if (fade_done(&s->out)) s->active = 0;
    }
    mix[c] = saturate(left);
    if (pair) mix[c + 1] = saturate(right);
    l->mixing = !fade_done(&l->in) || l->streams[0].active || l->streams[1].active || l->runout_phase || l->mode == MODE_WAIT;
}

/* Keep the voice where it is for the next frame: it waits, silent, for the
 * next hit. */
COLD static void hold(struct lock *l, u32 c, u32 pair, s64 now, s32 step)
{
    u64 at = (u64)(now - ((s64)step << STEP_TO_Q32));
    playhead_set(c, at);
    if (pair) playhead_set(c + 1, at);
    l->expect = (s64)at;
}

static void disengage(struct lock *l) { memset(l, 0, sizeof(*l)); }

/* Start following the voice where it is. */
COLD static void engage(struct lock *l, const struct lock_zone *z, u32 zone, s64 at, u32 own_q16)
{
    disengage(l);
    l->engaged = 1; l->zone = (u8)zone;
    l->timeline = l->expect = at;
    slice_start(l, z, (u32)(at >> 32));
    slice_end(l, z);
    slice_plan(l, z, own_q16);
    l->o = (s32)output_frames(z, at - l->a);
    l->in.phase = 1u << 16;
    ++timelock_stats.resyncs;
}

/* The next slice, a fill piece, the run-out, or holding the voice. */
COLD static void event(struct lock *l, u32 c, u32 pair, const struct lock_zone *z, const struct lock_table *t, s64 now, s32 step)
{
    s64 at = now + l->lap, per = (s64)step << STEP_TO_Q32;
    if (l->timeline >= l->onset_at) {
        if (!l->b_hit) { /* the zone's end: play out, or stay held when the slice ran out */
            l->onset_at = ONSET_NONE;
            if (l->mode != MODE_WAIT) { l->mode = MODE_END; l->due = DUE_NONE; return; }
            hold(l, c, pair, now, step);
            return;
        }
        /* The voice reaches the hit on time: it reads the frames before the
         * hit, which round the loop's end are the loop's last. */
        s64 early = l->b - l->timeline; /* sample frames to the hit at the sample's speed */
        s64 ratio = (s64)(((s64)step * z->inverse) >> 32); /* Q24 step / base */
        s64 to = l->b - (((early >> 16) * ratio) >> 8);
        int heard = l->mode != MODE_WAIT;
        u32 law = l->plan.key == l->serial && l->plan.kind == PLAN_ONSET ? l->plan.law : 0u;
        ++l->serial;
        l->a = l->b; l->a_lap = l->b_lap; l->cursor = l->b_index + 1;
        slice_end(l, z);
        slice_plan(l, z, t->own[c]);
        l->o = -(s32)output_frames(z, early);
        join(l, c, pair, z, now, to, PREROLL_FRAMES, law, heard);
        ++timelock_stats.onsets;
        return;
    }
    if (l->mode == MODE_WAIT) { hold(l, c, pair, now, step); return; }
    if (l->mode == MODE_RUNOUT) {
        /* The slice's own audio ends: fade it in time. */
        if (!l->runout_phase && step > 0 && at + (s64)RUNOUT_FRAMES * per >= l->b) {
            l->runout_phase = 1; l->mixing = 1; l->due = DUE_NONE; ++timelock_stats.runouts;
        }
        return;
    }
    if (l->mode != MODE_FILL || (u32)l->o >= l->length) { l->due = DUE_NONE; return; }
    /* A fill piece: back to the timeline, as near as the slice allows. */
    u32 left = l->length - (u32)l->o, piece = l->next_piece;
    int last = left < piece + piece / 2;
    u32 reads = last ? left : piece + l->xfade;      /* output frames the piece reads */
    s64 lo = l->a + (s64)(SETTLE_FRAMES < l->own / 3 ? SETTLE_FRAMES : l->own / 3) * per;
    s64 rise = last_rise(l, z, at);
    if (rise) {
        s64 guard = rise + (s64)PROTECT_FRAMES * ((s64)z->base << STEP_TO_Q32);
        if (guard > lo) lo = guard < at ? guard : at;
    }
    s64 hi = l->b - (s64)reads * per, to = l->timeline;
    u32 law = 0;
    if (l->plan.key == l->serial && l->plan.kind == PLAN_FILL) { to += l->plan.offset; law = l->plan.law; ++timelock_stats.planned; }
    ++l->serial;
    if (to < lo) to = lo;
    if (to > hi) to = hi;
    join(l, c, pair, z, now, to, l->xfade, law, 1);
    ++timelock_stats.fills;
    l->due = last ? DUE_NONE : l->o + (s32)piece;
    l->next_piece = jittered(l->piece);
}

extern const u8 timelock_events_start[], timelock_events_end[];

/* Preloads the next event's code and data into L2, a few lines a frame. */
static void warm(struct lock *l, const struct lock_zone *z)
{
    u32 code = (u32)(timelock_events_end - timelock_events_start), first = (u32)l->warmed * WARM_LINES * CACHE_LINE_BYTES;
    for (u32 i = 0; i < WARM_LINES; ++i) {
        u32 at = first + i * CACHE_LINE_BYTES;
        if (at < code) __builtin_prefetch(timelock_events_start + at);
        else {
            u32 data = (at - code) / CACHE_LINE_BYTES;
            if (data < sizeof(*l) / CACHE_LINE_BYTES) __builtin_prefetch((const u8 *)l + (data + 1u) * CACHE_LINE_BYTES);
            else { __builtin_prefetch(z->positions + l->cursor); __builtin_prefetch(z->positions + l->b_index); return; }
        }
    }
    ++l->warmed;
}

/* An event may run this frame: one a frame, and not once the frame's work
 * is heavy; it runs a frame later instead. */
static inline int room(void)
{
    if (!events || cpu_cycles() - frame_start > EVENT_BUDGET_CYCLES) return 0;
    --events;
    return 1;
}

/* ---- clocks ---- */

static inline int clock_live(const struct clock *k)
{
    return k->period_q8 && k->primed && k->age < CLOCK_LATE_PERIODS * (k->period_q8 >> 8);
}

static u32 frames_now; /* output frames counted while the hook runs */

/* A tick: the period averages the last `window` intervals; two in a row a
 * fifth off start a new average. */
static __attribute__((noinline)) void clock_tick(struct clock *k, u32 ticks)
{
    /* A tick after the clock stopped measures nothing: the gap is no interval. */
    int restart = !k->primed || (k->period_q8 && k->age >= CLOCK_LATE_PERIODS * (k->period_q8 >> 8));
    if (restart) k->count = 0, k->measuring = 0, k->off = 0;
    else {
        /* Two intervals in a row off the same way are a new tempo; one is a late tick. */
        u32 mean = k->period_q8 >> 8, margin = mean / CLOCK_SNAP_FRACTION, interval = k->age / ticks;
        s32 off = !k->period_q8 ? 0 : interval > mean + margin ? 1 : interval < mean - margin ? -1 : 0;
        if (!k->period_q8) k->count = 0, k->measuring = 1;
        /* A new tempo waits a beat of ticks: a few read together would measure too short. */
        else if (off && off == k->off && k->measuring) k->count = 0, k->measuring = 0;
        k->off = (signed char)off;
    }
    /* Each tick's time; ticks read together share it. */
    for (u32 n = 0; n < ticks; ++n) {
        if (!restart || n) k->count = (u8)(k->count < k->window ? k->count + 1 : k->window);
        k->times[k->next] = frames_now;
        k->next = (u8)((k->next + 1) & (CLOCK_RING - 1));
    }
    u32 last = (k->next - 1u) & (CLOCK_RING - 1);
    if (k->count && (k->measuring || k->count >= k->settle)) {
        u32 first = k->times[(last - k->count) & (CLOCK_RING - 1)];
        u32 period = ((frames_now - first) << 8) / k->count;
        if (period >= CLOCK_PERIOD_MIN_FRAMES << 8) k->period_q8 = period, k->measuring = 1;
    }
    k->primed = 1; k->seen += ticks; k->age = 0;
    s64 error = (s64)(((u64)k->seen << 32) - k->position);
    if (restart || error > (s64)(1ull << 32) || error < -(s64)(1ull << 32)) { k->position = (u64)k->seen << 32; ++k->epoch; }
    else k->position += (u64)(error >> CLOCK_PHASE_SHIFT);
}

/* A frame of a clock, with the ticks that arrived in it. After a gap the
 * first tick measures nothing: the clock runs at the period it had. */
static inline void clock_frame(struct clock *k, u32 ticks)
{
    if (++k->age >= CLOCK_GAP_FRAMES) k->primed = 0;
    u64 lead = (u64)(k->seen + CLOCK_LEAD_TICKS) << 32;
    if (k->primed && k->position + k->inverse < lead) k->position += k->inverse;
    if (ticks) clock_tick(k, ticks);
}

/* Advances the timeline: from the clock's position when it runs, else at the speed. */
static inline void timeline_frame(struct lock *l, const struct lock_zone *z, const struct clock *k)
{
    if (!k || !z->tick || !clock_live(k)) {
        l->anchored = 0;
        l->timeline += (s64)z->base << STEP_TO_Q32;
        return;
    }
    if (!l->anchored || l->epoch != k->epoch) {
        l->anchored = 1; l->epoch = k->epoch; l->position_at = k->position; l->tick_at = l->timeline;
    }
    s64 d = (s64)(k->position - l->position_at), whole = d >> 32;
    if (whole > 0) { l->tick_at += whole * z->tick; l->position_at += (u64)whole << 32; d -= whole << 32; }
    s64 next = l->tick_at + (((d >> 16) * (z->tick >> 8)) >> 8);
    if (next > l->timeline) l->timeline = next;
}

static void channel_frame(u32 c, s32 *mix, const s32 *steps, const struct lock_table *t)
{
    struct lock *l = &timelock_locks[c];
    u32 zone = REG32(ZONE_SELECTOR_BASE + c * ZONE_SELECTOR_STRIDE) & ZONE_SELECTOR_MASK;
    const struct lock_zone *z = &t->zone[c][zone];
    u32 pair = t->pair[c];
    if (!t->on[c] || !z->count || (s32)REG32(LEVEL_WORD_BASE + c * LEVEL_WORD_STRIDE) < LEVEL_SILENT) {
        if (l->engaged) disengage(l);
        return;
    }
    s32 step = steps[c];
    s64 now = (s64)playhead_get(c);
    l->expect += (s64)step << STEP_TO_Q32;
    if (looping(z) && l->expect >= frames_q32(z->turn)) { l->expect -= round_q32(z); l->lap += round_q32(z); } /* the voice comes round */
    s64 off = now - l->expect;
    if (!l->engaged || l->zone != zone || off > (s64)FOLLOW_Q32 || off < -(s64)FOLLOW_Q32) {
        engage(l, z, zone, now, t->own[c]);
        return;
    }
    l->expect = now;
    timeline_frame(l, z, t->clock[c] ? &timelock_clocks[t->clock[c] - 1] : 0);
    ++l->o;
    /* This frame was read where the voice was; a write moves the next. */
    if (l->mixing) mix_frame(l, mix, c, pair, z, step);
    if (l->timeline + (s64)WARM_FRAMES * ((s64)z->base << STEP_TO_Q32) >= l->onset_at || l->o + WARM_FRAMES >= l->due) warm(l, z);
    if (l->timeline >= l->onset_at) { if (room()) { l->warmed = 0; event(l, c, pair, z, t, now, step); } return; }
    if (l->o < l->due) return;
    /* A fill piece waits a frame while the channels' fades are full. */
    if (l->mode == MODE_FILL && streams_live >= FILL_STREAMS_MAX && l->waited < FILL_WAIT_MAX) { l->due = l->o + 1; ++l->waited; ++timelock_stats.waits; return; }
    l->waited = 0;
    if (room()) { l->warmed = 0; event(l, c, pair, z, t, now, step); }
}

/* The audio interrupt's frame, from grain mode's hook. */
void timelock_frame(s32 *mix, const s32 *steps)
{
    const struct lock_table *t = &timelock_tables[timelock_live];
    frame_start = cpu_cycles();
    events = EVENTS_PER_FRAME;
    ++frames_now;
    u32 live_streams = 0;
    /* The clocks: MIDI Clock bytes counted as they arrive, and each CV clock's rising edges. */
    struct clock *k = &timelock_clocks[0];
    u32 midi = patch_midi_clock_ticks, ticks = midi - k->source;
    clock_frame(k, ticks > CLOCK_FRAME_TICKS_MAX ? 1 : ticks); /* more is a backlog from before the hook ran */
    k->source = midi;
    for (u32 n = 1; n < CLOCKS; ++n) {
        if (t->clock_input[n] == CLOCK_INPUT_NONE) continue;
        k = &timelock_clocks[n];
        s32 v = ((volatile s16 *)EQ_CV_SAMPLES)[t->clock_input[n]];
        u32 rise = !k->high && v > CLOCK_HIGH;
        if (rise) k->high = 1; else if (k->high && v < CLOCK_LOW) k->high = 0;
        clock_frame(k, rise);
    }
    /* Ask for each channel's lines before any is needed. */
    for (u32 c = 0; c < PRESET_CHANNELS; ++c) {
        if (!t->on[c]) continue;
        const struct lock *l = &timelock_locks[c];
        __builtin_prefetch(l);
        __builtin_prefetch(&t->zone[c][REG32(ZONE_SELECTOR_BASE + c * ZONE_SELECTOR_STRIDE) & ZONE_SELECTOR_MASK]);
        if (l->mixing) { __builtin_prefetch(&l->streams[0]); live_streams += l->streams[0].active + l->streams[1].active; }
    }
    streams_live = live_streams;
    for (u32 c = 0; c < PRESET_CHANNELS; ++c) if (t->on[c]) channel_frame(c, mix, steps, t);
}

/* ---- foreground ---- */

static u32 active;

int timelock_active(void) { return active != 0; }

/* A Stereo/Right channel plays as its master and takes its settings. */
static u32 master(u32 channel)
{
    return channel && channel_record(channel)[CHANNEL_MODE] == CHANNEL_MODE_STEREO_RIGHT ? channel - 1 : channel;
}

/* A channel's Time Lock; a Stereo/Right channel's is its master's. */
int timelock_wanted(u32 channel) { return time_lock_channels[master(channel)]; }

static int digit(char c) { return c >= '0' && c <= '9'; }
static int letter(char c, char lower) { return (c | ASCII_LOWER) == lower; }

/* Auto Beats: a 60-200 BPM tempo in the sample's name, else the power of two
 * that puts the loop at GUESS_BPM_LOW or above. */
u32 timelock_guess(const u8 *sample, u32 span, u32 rate)
{
    const char *name = (const char *)sample;
    u32 found = 0;
    for (u32 i = 0; i < SAMPLE_NAME_BYTES && name[i];) {
        if (!digit(name[i])) { ++i; continue; }
        u32 start = i, value = 0;
        while (i < SAMPLE_NAME_BYTES && digit(name[i])) value = value * 10 + (u32)(name[i++] - '0');
        if (i - start > NAME_TEMPO_DIGITS || value < NAME_BPM_LOW || value > NAME_BPM_HIGH) continue;
        int marked = i + 3 <= SAMPLE_NAME_BYTES && letter(name[i], 'b') && letter(name[i + 1], 'p') && letter(name[i + 2], 'm');
        if (!found || marked) found = value;
        if (marked) break;
    }
    if (!span || !rate) return 1;
    if (found) {
        u32 beats = (u32)(((u64)found * span * 2 / ((u64)SECONDS_PER_MINUTE * rate) + 1) / 2);
        if (beats && beats <= TIME_LOCK_BEATS_MAX) return beats;
    }
    u32 beats = 1;
    while (beats < TIME_LOCK_BEATS_MAX && (u64)SECONDS_PER_MINUTE * beats * rate < (u64)GUESS_BPM_LOW * span) beats <<= 1;
    return beats;
}

/* The frames the zone's Beats span: its loop when it loops, else start to end. */
static u32 zone_span(const u8 *record, const u8 *z, u32 frames)
{
    u32 start = get_u32(z + ZONE_SAMPLE_START), end = get_u32(z + ZONE_SAMPLE_END);
    u32 loop = get_u32(z + ZONE_LOOP_START), length = get_u32(z + ZONE_LOOP_LENGTH) >> ZONE_LOOP_LENGTH_SHIFT;
    if (end > frames) end = frames;
    if (record[CHANNEL_LOOP_MODE] != CHANNEL_LOOP_OFF && loop >= start && length && loop + length <= end) return length;
    return end > start ? end - start : 0;
}

/* The channel whose tempo settings a channel plays at, and the tempo. */
static u32 tempo_channel(u32 c) { return time_lock_tempo[c] == TIME_LOCK_TEMPO_CHANNEL_1 ? 0 : c; }
static u32 tempo_of(u32 c)
{
    u32 tempo = time_lock_tempo[tempo_channel(c)];
    return tempo == TIME_LOCK_TEMPO_CHANNEL_1 ? TIME_LOCK_TEMPO_OWN : tempo;
}
static u32 clock_ppqn(u32 c)
{
    u32 s = tempo_channel(c);
    if (tempo_of(c) == TIME_LOCK_TEMPO_MIDI) return MIDI_PPQN;
    return time_lock_ppqn[s] ? time_lock_ppqn[s] : TIME_LOCK_PPQN_DEFAULT;
}
static struct clock *clock_of(u32 c)
{
    u32 tempo = tempo_of(c);
    return tempo == TIME_LOCK_TEMPO_MIDI ? &timelock_clocks[0] : tempo == TIME_LOCK_TEMPO_CV ? &timelock_clocks[1 + tempo_channel(c)] : 0;
}
/* Tenths of BPM from a clock's period, held while the clock stays within
 * BPM_HOLD_HUNDREDTHS of the tenth shown. */
static u32 clock_bpm10(struct clock *k, u32 ppqn)
{
    if (!k->period_q8) return 0;
    u32 raw = (u32)((u64)SECONDS_PER_MINUTE * HUNDREDTHS * OUTPUT_HZ * (1u << 8) / ((u64)k->period_q8 * ppqn));
    s32 off = (s32)raw - (s32)(k->shown10 * TENTHS);
    if (off > BPM_HOLD_HUNDREDTHS || off < -BPM_HOLD_HUNDREDTHS) k->shown10 = (raw + TENTHS / 2) / TENTHS;
    return k->shown10;
}

void timelock_tempo(u32 channel, u32 zone, struct timelock_tempo *out)
{
    u32 c = master(channel);
    const u8 *record = channel_record(c), *z = zone_record(c, zone);
    const u8 *sample = z[0] ? ready_sample(c, zone) : 0;
    memset(out, 0, sizeof(*out));
    struct clock *k = clock_of(c);
    out->clocked = k != 0;
    out->clock_live = k && clock_live(k);
    if (out->clock_live) out->bpm10 = clock_bpm10(k, clock_ppqn(c));
    if (!sample) return;
    u32 rate = REG32((u32)sample + SAMPLE_RATE), span = zone_span(record, z, REG32((u32)sample + SAMPLE_FRAMES));
    if (!span || !rate) return;
    out->guessed = !time_lock_beats[c][zone];
    out->beats = out->guessed ? timelock_guess(sample, span, rate) : time_lock_beats[c][zone];
    out->own_bpm10 = (u32)((u64)SECONDS_PER_MINUTE * TENTHS * out->beats * rate / span);
    u32 tempo = tempo_of(c);
    if (tempo == TIME_LOCK_TEMPO_OWN) out->bpm10 = out->own_bpm10;
    else if (tempo == TIME_LOCK_TEMPO_SPEED)
        out->bpm10 = (u32)((u64)out->own_bpm10 * (u32)(TIME_LOCK_SPEED_UNITY + time_lock_speed[tempo_channel(c)]) / TIME_LOCK_SPEED_UNITY);
}

/* A beat of a tempo in Q8 output frames, for the mix effects' delay: MIDI
 * clock's (channel PRESET_CHANNELS), or the tempo a channel's Time Lock
 * plays at. 0 when that clock has stopped or the channel has no tempo. */
u32 timelock_beat_q8(u32 channel)
{
    if (channel >= PRESET_CHANNELS) return clock_live(&timelock_clocks[0]) ? timelock_clocks[0].period_q8 * MIDI_PPQN : 0;
    u32 c = master(channel);
    const struct clock *k = clock_of(c);
    if (k) return clock_live(k) ? k->period_q8 * clock_ppqn(c) : 0;
    struct timelock_tempo t;
    timelock_tempo(c, REG32(ZONE_SELECTOR_BASE + c * ZONE_SELECTOR_STRIDE) & ZONE_SELECTOR_MASK, &t);
    return t.bpm10 ? (u32)((u64)SECONDS_PER_MINUTE * TENTHS * OUTPUT_HZ * (1u << 8) / t.bpm10) : 0;
}

/* A zone's sample range, as the voice plays it (grain mode's ranges), and
 * the timeline's speed at the channel's tempo. */
static void zone_build(struct lock_zone *out, u32 channel, u32 zone, u32 pair)
{
    memset(out, 0, sizeof(*out));
    const u8 *record = channel_record(channel), *z = zone_record(channel, zone);
    const u8 *sample = z[0] ? ready_sample(channel, zone) : 0;
    if (!sample || record[CHANNEL_REVERSE]) return;
    u32 count, analysed;
    const u32 *positions = slice_map_find(sample, &count, &analysed);
    if (!positions || !count) return;
    u32 side = pair ? 0 : z[ZONE_SIDE] & 1u;
    u32 left = REG32((u32)sample + SAMPLE_BUFFER_0 + side * 4u);
    u32 right = pair ? REG32((u32)sample + SAMPLE_BUFFER_0 + 4u) : 0;
    u32 frames = REG32((u32)sample + SAMPLE_FRAMES), rate = REG32((u32)sample + SAMPLE_RATE);
    u32 start = get_u32(z + ZONE_SAMPLE_START), end = get_u32(z + ZONE_SAMPLE_END);
    if (end > frames) end = frames;
    if (!left || start >= end || !rate) return;
    out->pcm[0] = (const s32 *)left + SAMPLE_GUARD_FRAMES;
    out->pcm[1] = right ? (const s32 *)right + SAMPLE_GUARD_FRAMES : 0;
    out->positions = positions; out->count = count;
    out->first = start; out->end = end < analysed ? end : analysed;
    if (out->end <= out->first + 2 * READ_MARGIN) { out->count = 0; return; }
    u32 loop = get_u32(z + ZONE_LOOP_START), length = get_u32(z + ZONE_LOOP_LENGTH) >> ZONE_LOOP_LENGTH_SHIFT;
    out->turn = out->end;
    if (record[CHANNEL_LOOP_MODE] != CHANNEL_LOOP_OFF && loop >= start && length && loop + length <= out->end) {
        out->loop = loop; out->turn = loop + length;
    }
    u64 own = ((u64)rate << AUDIO_STEP_SHIFT) / OUTPUT_HZ, base = own;
    u32 tempo = tempo_of(channel);
    if (tempo == TIME_LOCK_TEMPO_SPEED) base = own * (u32)(TIME_LOCK_SPEED_UNITY + time_lock_speed[tempo_channel(channel)]) / TIME_LOCK_SPEED_UNITY;
    else if (tempo != TIME_LOCK_TEMPO_OWN) {
        u32 span = zone_span(record, z, frames), beats = time_lock_beats[channel][zone];
        if (!beats) beats = timelock_guess(sample, span, rate);
        u64 ticks = (u64)beats * clock_ppqn(channel);
        out->tick = (s64)(((u64)span << 32) / ticks);
        const struct clock *k = clock_of(channel);
        /* Until the clock is measured: the sample's own speed. */
        if (k->period_q8) base = ((u64)span << (AUDIO_STEP_SHIFT + 8)) / (ticks * k->period_q8);
        if (base > own << SPEED_RANGE_SHIFT) base = own << SPEED_RANGE_SHIFT;
        if (base < (own >> SPEED_RANGE_SHIFT) + 1) base = (own >> SPEED_RANGE_SHIFT) + 1;
    }
    out->base = (s32)base;
    out->inverse = (u32)((1ull << 56) / (u32)out->base);
    out->loop_index = (u16)(out->loop ? timelock_after(out, out->loop - 1u) : 0u);
}

/* A zone's sample name, hashed (FNV-1a). */
static u32 name_hash(const u8 *zone)
{
    u32 h = FNV_BASIS;
    for (u32 i = 0; i < ZONE_NAME_BYTES && zone[i]; ++i) h = (h ^ zone[i]) * FNV_PRIME;
    return h;
}

/* A Beats set by hand belongs to the zone's sample: a new sample in the zone
 * puts it back to Auto. A preset load or record copy brings its own. */
static void beats_follow_samples(void)
{
    static u32 names[PRESET_CHANNELS][ZONES], loaded, copies, started;
    u32 now[PRESET_CHANNELS][ZONES], load = patch_grain_loaded, copy = records_copied_count;
    for (u32 c = 0; c < PRESET_CHANNELS; ++c)
        for (u32 z = 0; z < ZONES; ++z) now[c][z] = name_hash(zone_record(c, z));
    int adopt = !started || load != loaded || patch_grain_loaded != load
        || copy != copies || records_copied_count != copy;
    for (u32 c = 0; c < PRESET_CHANNELS; ++c)
        for (u32 z = 0; z < ZONES; ++z)
            if (!adopt && now[c][z] != names[c][z]) time_lock_beats[c][z] = 0;
    memcpy(names, now, sizeof(names));
    loaded = patch_grain_loaded; copies = records_copied_count; started = 1;
}

void timelock_foreground(void)
{
    beats_follow_samples();
    for (u32 n = 0; n < CLOCKS; ++n) {
        u32 ppqn = !n ? MIDI_PPQN : time_lock_ppqn[n - 1] ? time_lock_ppqn[n - 1] : TIME_LOCK_PPQN_DEFAULT;
        u32 window = ppqn * CLOCK_BEATS;
        timelock_clocks[n].window = (u8)(window < CLOCK_RING ? window : CLOCK_RING - 1);
        timelock_clocks[n].settle = (u8)(ppqn < CLOCK_RING ? ppqn : CLOCK_RING - 1);
    }
    for (u32 n = 0; n < CLOCKS; ++n) {
        struct clock *k = &timelock_clocks[n];
        u32 period = k->period_q8;
        k->inverse = period ? (u32)((1ull << 40) / period) : 0;
    }
    u32 next = timelock_live ^ 1u;
    struct lock_table *t = &timelock_tables[next];
    memset(t, 0, sizeof(*t));
    memset(t->clock_input, CLOCK_INPUT_NONE, sizeof(t->clock_input));
    u32 any = 0;
    for (u32 c = 0; c < PRESET_CHANNELS; ++c) {
        if (channel_record(c)[CHANNEL_MODE] == CHANNEL_MODE_STEREO_RIGHT) continue;
        /* The clock it follows: MIDI, or the CV clock of the channel whose tempo it takes.
         * Clocks run with Time Lock off too, so channel 1 holds the tempo others follow. */
        u32 tempo = tempo_of(c), from = tempo_channel(c);
        s32 input = eq_cv_input(time_lock_input[from] + EQ_CV_SOURCE_OFF, from);
        if (tempo == TIME_LOCK_TEMPO_MIDI) { t->clock[c] = 1; any = 1; }
        if (mix_fx_wants_midi_clock()) any = 1;
        else if (tempo == TIME_LOCK_TEMPO_CV && input >= 0) { t->clock[c] = (u8)(2 + from); t->clock_input[1 + from] = (u8)input; any = 1; }
        if (c == grain_playing_channel()) continue;
        t->pair[c] = c + 1 < PRESET_CHANNELS && channel_record(c + 1)[CHANNEL_MODE] == CHANNEL_MODE_STEREO_RIGHT;
        if (t->pair[c] && grain_playing_channel() == c + 1) continue;
        for (u32 z = 0; z < ZONES; ++z)
            if (timelock_wanted(c)) { zone_build(&t->zone[c][z], c, z, t->pair[c]); t->on[c] = 1; any = 1; }
    }
    /* The slice's own audio against its length follows the pitch: 0 when
     * the voice stands still or runs backwards. */
    for (u32 c = 0; c < PRESET_CHANNELS; ++c) {
        if (!t->on[c]) continue;
        s32 step = patch_grain_steps[c];
        const struct lock_zone *z = &t->zone[c][REG32(ZONE_SELECTOR_BASE + c * ZONE_SELECTOR_STRIDE) & ZONE_SELECTOR_MASK];
        t->own[c] = step > 0 && z->base ? (u32)(((u64)z->base << 16) / (u32)step) : 0;
    }
    if (memcmp(t, &timelock_tables[timelock_live], sizeof(*t))) {
        MEMORY_BARRIER();
        timelock_live = next;
    }
    if (any != active) { active = any; audio_hook_update(); }
    for (u32 c = 0; c < PRESET_CHANNELS; ++c)
        if (timelock_tables[timelock_live].on[c]) joins_plan(c, &timelock_tables[timelock_live]);
}

/* Set parameter SET_TIME_LOCK + field (PROTOCOL.md); replies the stored value. */
u32 timelock_set(u32 channel, u32 zone, u32 field, s32 value, u8 *out, u32 *count)
{
    if (channel >= PRESET_CHANNELS || zone > ZONES) return BAD_RANGE;
    u32 c = master(channel);
    s32 stored;
    switch (field) {
    case TIMELOCK_SET_PLAYING: {
        struct timelock_tempo tempo;
        timelock_tempo(channel, zone < ZONES ? zone : 0, &tempo);
        memcpy(out, &timelock_stats, sizeof(timelock_stats));
        memcpy(out + sizeof(timelock_stats), &tempo, sizeof(tempo));
        *count = sizeof(timelock_stats) + sizeof(tempo);
        return OK;
    }
    case TIMELOCK_SET_ON: stored = time_lock_channels[c] = value != 0; break;
    case TIMELOCK_SET_TEMPO: stored = time_lock_tempo[c] = (u8)clamp(value, 0, TIME_LOCK_TEMPOS - 1); break;
    case TIMELOCK_SET_SPEED: stored = time_lock_speed[c] = (s16)clamp(value, TIME_LOCK_SPEED_MIN, TIME_LOCK_SPEED_MAX); break;
    case TIMELOCK_SET_INPUT:
        time_lock_input[c] = (u8)(clamp(value, EQ_CV_SOURCE_OFF, EQ_CV_SOURCE_MAX) - EQ_CV_SOURCE_OFF);
        stored = time_lock_input[c] + EQ_CV_SOURCE_OFF;
        break;
    case TIMELOCK_SET_PPQN: stored = time_lock_ppqn[c] = (u8)clamp(value, 0, TIME_LOCK_PPQN_MAX); break;
    case TIMELOCK_SET_BEATS:
        if (zone >= ZONES) return BAD_RANGE;
        stored = time_lock_beats[c][zone] = (u8)clamp(value, 0, TIME_LOCK_BEATS_MAX);
        break;
    default: return BAD_RANGE;
    }
    put_u32(out, (u32)stored);
    *count = 4;
    return OK;
}

void timelock_init(void)
{
    /* sin(pi/2 i/256) by rotation in Q30: cos and sin of the step. */
    s64 c = 1073721611, s = 6588356, x = ONE_Q30, y = 0;
    for (u32 i = 0; i <= SINE_STEPS; ++i) {
        sine[i] = (u32)(y < 0 ? 0 : y > ONE_Q30 ? ONE_Q30 : y);
        s64 nx = (x * c - y * s) >> 30, ny = (x * s + y * c) >> 30;
        x = nx; y = ny;
    }
    sine[SINE_STEPS] = ONE_Q30;
    for (u32 n = 1; n <= FADES_MAX; ++n) reciprocal[n] = (1u << 16) / n;
    reciprocal[0] = 1u << 16;
    memset(timelock_locks, 0, sizeof(timelock_locks));
    timelock_live = 0; active = 0;
}
