#include "ext.h"
#include "mix_fx.h"
#include "mix_tables.h"

/* Mix effects in the audio interrupt: integer only (the hooks are built
 * without NEON). Levels and gain reductions are log2 values in Q24
 * (1 << 24 is an octave, 6.02 dB). */

#define Q24_OCTAVE         (1 << 24)
#define DB_TO_Q24(db)      ((s32)((db) * 2786918))        /* 2^24 / 6.0206 */
#define LEVEL_FLOOR        (-24 * Q24_OCTAVE)
#define GAIN_ONE_Q24       (1 << 24)
#define FRAME_HZ           48000u
#define REVERB_HZ          32000u
#define REVERB_WORDS       32768u
#define REVERB_MASK        (REVERB_WORDS - 1u)
#define DELAY_WORDS        65536u
#define DELAY_MASK         (DELAY_WORDS - 1u)
#define DELAY_MIN_FRAMES   64u
#define DELAY_GLIDE_SHIFT  11      /* the delay time follows a change over about 40 ms */
#define DIVISION_UNITS     48u     /* a division counts 48ths of a beat */
#define DELAY_RETARGET_MASK 255u   /* the delay follows the clock every 256 frames */
#define SOFT_CLIP_LIMIT    49152   /* 1.5 in Q15: the delay loop's clip reaches 1.0 here */
#define PERCENT_Q15        328     /* 1 % in Q15 */

struct mix_settings mix_defaults;

void mix_channel_get(const struct mix_settings *from, u32 c, struct mix_channel *out)
{
    out->comp = from->comp[c];
    out->reverb_send = from->reverb_send[c];
    out->delay_send = from->delay_send[c];
    out->send_pre = from->send_pre[c];
    memcpy(out->send_route, from->send_route[c], sizeof(out->send_route));
}
/* Unchanged settings stay as they are, so a copy of defaults leaves a
 * preset without its own settings unmodified. */
void mix_channel_put(u32 c, const struct mix_channel *in)
{
    struct mix_channel now;
    mix_channel_get(mix_now(), c, &now);
    if (!memcmp(&now, in, sizeof(now))) return;
    struct mix_settings *m = mix_edit();
    m->comp[c] = in->comp;
    m->reverb_send[c] = in->reverb_send;
    m->delay_send[c] = in->delay_send;
    m->send_pre[c] = in->send_pre;
    memcpy(m->send_route[c], in->send_route, sizeof(in->send_route));
    mix_fx_update();
}

struct mix_settings *mix_edit(void)
{
    struct mix_settings *live = (struct mix_settings *)patch_live.extension;
    if (live->format != MIX_FORMAT) { *live = mix_defaults; live->format = MIX_FORMAT; }
    return live;
}

const char *const mix_ratio_names[MIX_RATIOS] = {"Off", "1.5:1", "2:1", "3:1", "4:1", "6:1", "10:1", "20:1", "Limit"};
/* 1 - 1/ratio, Q16 */
static const u32 ratio_slope[MIX_RATIOS] = {0, 21845, 32768, 43691, 49152, 54613, 58982, 62259, 65536};
const u16 mix_attack_hundredths_ms[MIX_ATTACKS] = {5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000};
const u16 mix_release_ms[MIX_RELEASES] = {20, 50, 100, 200, 500, 1000, 2000};
const char *const mix_division_names[MIX_DIVISIONS] = {"1/16", "1/8T", "1/16D", "1/8", "1/4T", "1/8D", "1/4", "1/2T", "1/4D", "1/2", "1/1"};
static const u8 division_units[MIX_DIVISIONS] = {12, 16, 18, 24, 32, 36, 48, 64, 72, 96, 192};

/* The DEP-5 reverb's network, in 32 kHz samples: the pre-delay, four
 * allpasses and eight damped combs, and the taps each output sums. */
#define PREDELAY        294u
#define ALLPASSES       4u
#define COMBS           8u
#define ALLPASS_GAIN    19661   /* 0.6 */
#define REVERB_IN_GAIN  9830    /* 0.3 */
#define COMB_IN_GAIN    7248    /* 0.2212 */
#define OUTPUT_GAIN     27313   /* 0.8335 * 2.0, applied as Q14 */
#define COMB_SHIFT      3       /* the combs run 18 dB hotter than the DEP-5's for 16-bit cells; their taps undo it */
#define EARLY_TAPS      8u
#define LATE_TAPS       8u
static const u16 allpass_len[ALLPASSES] = {1197, 715, 211, 53};
static const u16 comb_len[COMBS] = {1117, 1281, 1470, 1686, 1934, 2219, 2546, 2921};
/* A tap: a line (0-7 a comb, 8 the input tape), a delay into it, a gain (Q14). */
struct tap { u8 line; u16 delay; s16 gain; };
#define INPUT_TAPE 8u
static const struct tap taps[2][LATE_TAPS + EARLY_TAPS] = {
    { {2, 1470, 16673}, {4, 1934, 19040}, {6, 2546, 21696}, {4, 967, 19040}, {7, 1460, -3280},
      {0, 558, 14575}, {2, 735, 16673}, {0, 1117, 14575},
      {INPUT_TAPE, 358, 256}, {INPUT_TAPE, 785, 696}, {INPUT_TAPE, 1018, -344}, {INPUT_TAPE, 1262, -688},
      {INPUT_TAPE, 1557, 600}, {INPUT_TAPE, 2335, 655}, {INPUT_TAPE, 3252, 632} },
    { {3, 1686, 17791}, {5, 2219, 20319}, {7, 2921, -3280}, {5, 1109, 20319}, {6, 1273, 21696},
      {1, 640, 15584}, {3, 843, 17791}, {1, 1281, 15584},
      {INPUT_TAPE, 358, 256}, {INPUT_TAPE, 736, -705}, {INPUT_TAPE, 1018, -600}, {INPUT_TAPE, 1342, -680},
      {INPUT_TAPE, 1557, 311}, {INPUT_TAPE, 2097, -664}, {INPUT_TAPE, 3062, -641}, {INPUT_TAPE, 4223, -624} },
};
#define TAPE_LEN 4224u
#define LOG2_THOUSAND_Q24 167201000 /* 3 log2(10) = log2(1000), Q24 */
static const u8 tap_count[2] = {LATE_TAPS + EARLY_TAPS - 1, LATE_TAPS + EARLY_TAPS};

struct comp_state { s32 gr; };
struct comp_coef { u32 slope, attack, release; s32 threshold, makeup, mix_q15; u8 on, drive; };
/* Drive curves, (1 + k) v / (1 + k v) for v in 0..1 in 256 steps, Q16;
 * built in the foreground when a Drive changes. k doubles every 20 % of
 * Drive, up to 31 (+30 dB for small signals) at 100. */
#define DRIVE_STEPS        256u
#define DRIVE_OCTAVES_Q24  (5 << 24)
static u16 drive_curve[MIX_CHANNELS + 1][DRIVE_STEPS + 1];
static u8 drive_built[MIX_CHANNELS + 1];

static s16 reverb_ram[REVERB_WORDS];
static s16 delay_ram[2][DELAY_WORDS];
static u32 reverb_base;
static u16 line_start[INPUT_TAPE + 1]; /* each line's write offset */
static u16 allpass_start[ALLPASSES];
static s32 comb_gain[COMBS], comb_damp[COMBS], comb_lp[COMBS];
static s32 down_hist[MIX_DOWN_TAPS], up_hist[2][MIX_UP_TAPS];
static u32 phase;
static s32 reverb_level_q15, delay_level_q15;
static s32 send_reverb, send_delay;   /* this frame's sends, Q31, from the channel hook */
/* Sidechain sources this frame, as MIX_KEY_* numbers: the channels before their compressors, the input. */
static s32 keys[MIX_KEY_INPUT + 1];
static struct comp_coef coef[MIX_CHANNELS + 1];
static struct comp_state state[MIX_CHANNELS + 1];
static u32 delay_write, delay_q8[2], delay_target_q8;
static s32 feedback_q15, low_coef, high_coef, low_state[2], high_state[2];
static u32 frames;
static volatile u32 beat_q8;   /* the delay tempo's beat, Q8 frames, from the foreground; 0: none */

/* ---- log and exp ---- */

static inline s32 log2_q24(u32 x) /* x > 0 */
{
    u32 lead = 31u - (u32)__builtin_clz(x);
    u32 frac = lead >= 12 ? x >> (lead - 12) : x << (12 - lead);  /* 1.12 mantissa */
    u32 i = (frac >> 6) & 63u, f = frac & 63u;
    u32 l = mix_log2_table[i] + (((mix_log2_table[i + 1] - mix_log2_table[i]) * f) >> 6);
    return (s32)(lead << 24) + (s32)(l << 8);
}

/* 2^(x / 2^24) in Q24, x at most 6 octaves; 0 below 24 octaves down. */
static inline s32 exp2_q24(s32 x)
{
    if (x < LEVEL_FLOOR) return 0;
    s32 whole = x >> 24;
    u32 frac = (u32)x & 0xffffffu, i = frac >> 18, f = (frac >> 10) & 255u;
    u32 m = mix_exp2_table[i] + (u32)(((u64)(mix_exp2_table[i + 1] - mix_exp2_table[i]) * f) >> 8); /* Q30 */
    s32 shift = 6 - whole;  /* Q30 to Q24 */
    return shift >= 0 ? (s32)(m >> shift) : (s32)(m << -shift);
}

static inline s32 smooth(s32 at, s32 target, u32 coef_q31) { return at + (s32)(((s64)(target - at) * coef_q31) >> 31); }
static inline s32 sat32(s64 v) { return v > 0x7fffffff ? 0x7fffffff : v < -0x7fffffff ? -0x7fffffff : (s32)v; }
static inline s32 sat16(s32 v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }

/* ---- compressor ---- */

/* A one-pole step for a time constant in hundredths of a millisecond, Q31: 1 / (1 + frames). */
static u32 time_coef(u32 hundredths_ms) { return (u32)(((u64)0x80000000u * 100u) / (100u + (u64)hundredths_ms * (FRAME_HZ / 1000u))); }

static void comp_coef_set(struct comp_coef *k, const struct mix_comp_set *s)
{
    k->on = s->ratio != 0;
    k->slope = ratio_slope[s->ratio < MIX_RATIOS ? s->ratio : 0];
    k->threshold = -DB_TO_Q24(s->threshold);
    k->makeup = DB_TO_Q24(s->gain);
    k->mix_q15 = s->mix * PERCENT_Q15;
    k->drive = s->drive;
    k->attack = time_coef(mix_attack_hundredths_ms[s->attack < MIX_ATTACKS ? s->attack : 0]);
    k->release = time_coef(100u * mix_release_ms[s->release < MIX_RELEASES ? s->release : 0]);
}

/* The peak detector, a hard knee, then the gain to apply, Q24. */
static inline s32 comp_gain(const struct comp_coef *k, struct comp_state *st, s32 key)
{
    u32 a = key < 0 ? (u32)-(key + 1) : (u32)key;
    s32 over = (a > 0 ? log2_q24(a) - (31 << 24) : LEVEL_FLOOR) - k->threshold;
    s32 target = over > 0 ? (s32)(((s64)over * k->slope) >> 16) : 0;
    st->gr = smooth(st->gr, target, target > st->gr ? k->attack : k->release);
    return exp2_q24(k->makeup - st->gr);
}

/* Drive: the curve, odd-symmetric, interpolated. */
static inline s32 drive(const u16 *curve, s32 x)
{
    u32 a = x < 0 ? (u32)-(x + 1) : (u32)x;
    u32 i = a >> 23, f = (a >> 15) & 255u;
    u32 y = curve[i] + (u32)(((s32)(curve[i + 1] - curve[i]) * (s32)f) >> 8);
    return x < 0 ? -(s32)(y << 15) : (s32)(y << 15);
}

static void drive_build(u16 *curve, u32 amount)
{
    /* k = 2^(5 amount / 100) - 1, Q16 */
    u64 k = ((u64)(u32)exp2_q24((s32)((s64)DRIVE_OCTAVES_Q24 * amount / MIX_PERCENT_MAX)) >> 8) - 65536u;
    for (u32 i = 0; i <= DRIVE_STEPS; ++i) {
        u64 v = (u64)i << 8;                          /* Q16 */
        u64 y = ((65536u + k) * v) / (65536u + ((k * v) >> 16));
        curve[i] = (u16)(y > 65535u ? 65535u : y);
    }
}

/* Parallel compression: the dry signal and the compressed one by Mix. */
static inline s32 blend(s32 dry, s32 wet, s32 mix_q15) { return dry + (s32)(((s64)(wet - dry) * mix_q15) >> 15); }

static inline s32 apply(s32 x, s32 gain_q24) { return sat32(((s64)x * gain_q24) >> 24); }

/* ---- reverb ---- */

/* Q15 products truncated toward zero, so a tail cannot sustain itself at
 * the last bits as rounding lets it. */
static inline s32 mul15(s32 a, s32 b) { return (a * b) / 32768; }
static inline s16 cell(u32 offset) { return reverb_ram[(reverb_base + offset) & REVERB_MASK]; }
static inline void cell_set(u32 offset, s32 v) { reverb_ram[(reverb_base + offset) & REVERB_MASK] = (s16)sat16(v); }
/* A value written at `start` reads back at start + delay after `delay` ticks
 * (the base moves down one cell a tick). */
static inline s32 line_read(u32 start, u32 delay) { return cell(start + delay); }

/* The algorithms: the DEP-5's number, the network, wander, and the chorus's input. */
enum { NET_NONE, NET_HALL, NET_GATED, NET_SPARSE };
enum { CHORUS_NONE, CHORUS_SEND, CHORUS_REVERB, CHORUS_REVERB_ONLY };
struct algo { u8 net, wander, chorus; };
static const struct algo algos[MIX_ALGOS] = {
    {NET_NONE, 0, CHORUS_SEND},          /* 1 chorus */
    {NET_HALL, 0, CHORUS_NONE},          /* 2 reverb */
    {NET_HALL, 1, CHORUS_NONE},          /* 3 reverb mod */
    {NET_HALL, 0, CHORUS_REVERB_ONLY},   /* 4 chorus to reverb: the chorused reverb only */
    {NET_HALL, 0, CHORUS_SEND},          /* 5 reverb, chorus parallel */
    {NET_HALL, 0, CHORUS_REVERB},        /* 6 reverb, chorus series/parallel */
    {NET_GATED, 0, CHORUS_NONE},         /* 7 nonlinear */
    {NET_GATED, 1, CHORUS_NONE},         /* 8 nonlinear mod */
    {NET_GATED, 0, CHORUS_REVERB},       /* 9 nonlinear chorus */
    {NET_SPARSE, 1, CHORUS_NONE},        /* 11 low density nonlinear mod */
};
const char *const mix_algo_names[MIX_ALGOS] = {"1 Chorus", "2 Reverb", "3 Rev Mod", "4 Cho Rev", "5 Rev+Cho",
    "6 Ser/Par", "7 NonLin", "8 NL Mod", "9 NL Chorus", "11 LD NL Mod"};
u32 mix_algo_chorus(u32 algo) { return algo < MIX_ALGOS && algos[algo].chorus != CHORUS_NONE; }

/* The DEP-5's nonlinear network: a diffuser (four allpasses; one for low
 * density) into a tape, read by ten taps a side out to 125 ms at Gate 125. */
#define GATED_ALLPASSES   4u
#define GATED_TAPS        10u
#define GATED_TAPE_LEN    16384u   /* taps reach 4000 x 4 at the 500 ms gate */
#define GATED_IN_GAIN     9830     /* 0.3 */
#define GATED_TAPE_GAIN   6144     /* 0.375 x 0.5 */
#define SPARSE_TAPE_GAIN  12288    /* 0.375 */
#define GATE_MS_PER_TENTH 5u       /* Decay 2.5 s reads as a 125 ms gate */
#define GATE_BASE_MS      125u
static const u16 gated_allpass_len[GATED_ALLPASSES] = {263, 159, 89, 69};
static const s16 gated_allpass_gain[GATED_ALLPASSES] = {14896, 16384, 16384, 16384};
#define SPARSE_ALLPASS_GAIN 14031  /* 0.4282 */
static const u16 gated_delay[2][GATED_TAPS] = {
    {333, 622, 922, 1222, 1558, 1927, 2297, 2722, 3187, 3707},
    {483, 760, 1061, 1396, 1742, 2112, 2517, 2956, 3436, 4000},
};
static const s16 gated_gain[2][GATED_TAPS] = {   /* Q12: 2.0 falling to 1.78 */
    {8192, 8088, 7992, 7888, 7792, 7696, 7600, 7504, 7408, 7320},
    {8144, 8040, 7936, 7840, 7744, 7648, 7552, 7456, 7360, 7272},
};

/* Chorus: two reads of one line sweep against each other by a triangle,
 * left longer while right is shorter, around 2.8 ms (the DEP-5 board's
 * MB654119 setting). */
#define CHORUS_WORDS      1024u
#define CHORUS_MASK       (CHORUS_WORDS - 1u)
#define CHORUS_BASE_Q8    (90u << 8)
#define CHORUS_DEPTH_MAX  160u     /* samples of sweep at Depth 100: 5 ms */
#define CHORUS_RATE_LOW   10737u   /* 0.08 Hz as a 32-bit phase step at 32 kHz */
#define CHORUS_RATE_OCTAVES 100147000 /* 5.97 octaves, Q24: Rate 100 is 5 Hz */
static s16 chorus_ram[CHORUS_WORDS];
static u32 chorus_write, chorus_phase, chorus_step, chorus_depth_q8;

/* Wander for the Mod algorithms: the DEP-5 stretches its frame with
 * filtered noise. Here every long read moves by the same small fraction,
 * which drifts toward a new random target every 4096 ticks. */
#define WANDER_PERIOD     4096u
#define WANDER_RANGE      164      /* +-0.25 %, Q16 */
#define WANDER_SHIFT      11
#define WANDER_ROOM       16u      /* cells: 0.25 % of the longest comb, and the interpolation's next */
static s32 wander, wander_target;
static u32 wander_seed = 0x2545f491u, wander_count;

static u32 algo_now = MIX_ALGOS, clearing;
static u32 gated_scale_q8 = 256u;
static u16 gated_allpass_start[GATED_ALLPASSES], gated_tape_start;
static s32 gated_lp;

#define CLEAR_WORDS       1024u    /* a changed algorithm clears its memory this much a tick */

static void reverb_layout(void)
{
    u32 at = 0;
    line_start[INPUT_TAPE] = (u16)at; at += TAPE_LEN + 1;
    for (u32 i = 0; i < ALLPASSES; ++i) { allpass_start[i] = (u16)at; at += allpass_len[i] + 1u; }
    /* Each comb has room past its length for the wander's longest read. */
    for (u32 i = 0; i < COMBS; ++i) { line_start[i] = (u16)at; at += comb_len[i] + 1u + WANDER_ROOM; }
    /* The gated network shares the memory: an algorithm change clears it. */
    at = 0;
    for (u32 i = 0; i < GATED_ALLPASSES; ++i) { gated_allpass_start[i] = (u16)at; at += gated_allpass_len[i] + 1u; }
    gated_tape_start = (u16)at;
}

/* A read `delay` back, moved by the wander (Q16 of the delay), interpolated. */
static inline s32 line_read_wander(u32 start, u32 delay)
{
    s32 q8 = (s32)(delay << 8) + (s32)((s32)delay * wander >> 8);
    u32 whole = (u32)q8 >> 8, f = (u32)q8 & 255u;
    s32 a = cell(start + whole), b = cell(start + whole + 1u);
    return a + (((b - a) * (s32)f) >> 8);
}

static void hall_tick(s32 in, s32 *left, s32 *right, u32 wandering)
{
    cell_set(line_start[INPUT_TAPE], in);
    s32 x = mul15(line_read(line_start[INPUT_TAPE], PREDELAY), REVERB_IN_GAIN);
    for (u32 i = 0; i < ALLPASSES; ++i) {
        s32 d = line_read(allpass_start[i], allpass_len[i]);
        s32 w = sat16(x - mul15(d, ALLPASS_GAIN));
        cell_set(allpass_start[i], w);
        x = d + mul15(w, ALLPASS_GAIN);
    }
    x = mul15(x, COMB_IN_GAIN) << COMB_SHIFT;
    for (u32 i = 0; i < COMBS; ++i) {
        s32 d = wandering ? line_read_wander(line_start[i], comb_len[i]) : line_read(line_start[i], comb_len[i]);
        comb_lp[i] = d + mul15(comb_lp[i] - d, comb_damp[i]);
        cell_set(line_start[i], x + mul15(comb_lp[i], comb_gain[i]));
    }
    for (u32 side = 0; side < 2; ++side) {
        s32 late = 0, early = 0;
        for (u32 t = 0; t < LATE_TAPS; ++t) {
            const struct tap *p = &taps[side][t];
            late += line_read(line_start[p->line], p->delay) * p->gain;
        }
        for (u32 t = LATE_TAPS; t < tap_count[side]; ++t) {
            const struct tap *p = &taps[side][t];
            early += line_read(line_start[p->line], p->delay) * p->gain;
        }
        s32 out = ((late >> COMB_SHIFT) + early) >> 14;
        *(side ? right : left) = sat16((out * OUTPUT_GAIN) >> 14);
    }
}

static void gated_tick(s32 in, s32 *left, s32 *right, u32 sparse, u32 wandering)
{
    /* Damp: a low-pass ahead of the diffuser. */
    s32 x = mul15(in, GATED_IN_GAIN);
    gated_lp = x + mul15(gated_lp - x, comb_damp[COMBS - 1]);
    x = gated_lp;
    u32 passes = sparse ? 1u : GATED_ALLPASSES;
    for (u32 i = 0; i < passes; ++i) {
        s32 g = sparse ? SPARSE_ALLPASS_GAIN : gated_allpass_gain[i];
        s32 d = line_read(gated_allpass_start[i], gated_allpass_len[i]);
        s32 w = sat16(x - mul15(d, g));
        cell_set(gated_allpass_start[i], w);
        x = d + mul15(w, g);
    }
    cell_set(gated_tape_start, mul15(x, sparse ? SPARSE_TAPE_GAIN : GATED_TAPE_GAIN) << COMB_SHIFT);
    for (u32 side = 0; side < 2; ++side) {
        s32 sum = 0;
        for (u32 t = 0; t < GATED_TAPS; ++t) {
            u32 delay = (gated_delay[side][t] * gated_scale_q8) >> 8;
            s32 v = wandering ? line_read_wander(gated_tape_start, delay) : line_read(gated_tape_start, delay);
            sum += v * gated_gain[side][t];
        }
        *(side ? right : left) = sat16((((sum >> 12) >> COMB_SHIFT) * OUTPUT_GAIN) >> 14);
    }
}

static inline s32 chorus_read(u32 q8)
{
    u32 whole = q8 >> 8, f = q8 & 255u;
    s32 a = chorus_ram[(chorus_write - whole) & CHORUS_MASK], b = chorus_ram[(chorus_write - whole - 1u) & CHORUS_MASK];
    return a + (((b - a) * (s32)f) >> 8);
}

/* One 32 kHz tick of the selected algorithm: Q15 in, Q15 out. */
static void reverb_tick(s32 in, s32 *left, s32 *right)
{
    u32 a = mix_now()->reverb_algo < MIX_ALGOS ? mix_now()->reverb_algo : 1u;
    if (a != algo_now) { algo_now = a; clearing = REVERB_WORDS / CLEAR_WORDS; }
    if (clearing) {
        /* Silent while the memory clears, a slice a tick. */
        --clearing;
        memset(&reverb_ram[clearing * CLEAR_WORDS], 0, CLEAR_WORDS * sizeof(reverb_ram[0]));
        for (u32 i = 0; i < COMBS; ++i) comb_lp[i] = 0;
        gated_lp = 0;
        *left = *right = 0;
        return;
    }
    const struct algo *alg = &algos[a];
    if (alg->wander) {
        if (++wander_count >= WANDER_PERIOD) {
            wander_count = 0;
            wander_seed = wander_seed * 1664525u + 1013904223u;
            wander_target = (s32)(wander_seed >> 16) % (2 * WANDER_RANGE + 1) - WANDER_RANGE;
        }
        wander += (wander_target - wander) >> WANDER_SHIFT;
    }
    s32 l = 0, r = 0;
    if (alg->net == NET_HALL) hall_tick(in, &l, &r, alg->wander);
    else if (alg->net != NET_NONE) gated_tick(in, &l, &r, alg->net == NET_SPARSE, alg->wander);
    reverb_base = (reverb_base - 1u) & REVERB_MASK;
    if (alg->chorus != CHORUS_NONE) {
        chorus_ram[chorus_write] = (s16)(alg->chorus == CHORUS_SEND ? in : sat16((l + r) >> 1));
        chorus_phase += chorus_step;
        u32 tri = chorus_phase < 0x80000000u ? chorus_phase >> 15 : (~chorus_phase) >> 15;  /* 0..65535 */
        u32 sweep_l = (u32)(((u64)chorus_depth_q8 * tri) >> 16), sweep_r = chorus_depth_q8 - sweep_l;
        s32 cl = chorus_read(CHORUS_BASE_Q8 + sweep_l), cr = chorus_read(CHORUS_BASE_Q8 + sweep_r);
        chorus_write = (chorus_write + 1u) & CHORUS_MASK;
        if (alg->chorus == CHORUS_REVERB_ONLY) { l = 0; r = 0; }
        l = sat16(l + cl); r = sat16(r + cr);
    }
    *left = l; *right = r;
}

/* Comb gains for the decay time (10^(-3 L / (T fs)), as the DEP-5) and
 * damping in proportion to each comb's length (the DEP-5's at Damp 20). */
static u32 comb_decay_k[COMBS], comb_damp_k[COMBS];
static void reverb_constants(void)
{
    for (u32 i = 0; i < COMBS; ++i) {
        comb_decay_k[i] = (u32)((u64)LOG2_THOUSAND_Q24 * comb_len[i] * 10u / REVERB_HZ); /* x tenths of a second */
        comb_damp_k[i] = comb_len[i] * 429u / 400u;
    }
}
static void reverb_update(u32 tenths, u32 damp)
{
    for (u32 i = 0; i < COMBS; ++i) {
        /* log2 of the gain: -3 log2(10) L / (T fs), Q24 */
        s32 g = exp2_q24(-(s32)(comb_decay_k[i] / tenths)); /* Q24, below one */
        comb_gain[i] = -(g >> 9);   /* Q15, negative as the DEP-5's */
        /* a = L * 1.31e-5 * (damp / 20)^2, Q15, at most 0.9 */
        u32 a = comb_damp_k[i] * damp * damp / 1000u;
        comb_damp[i] = (s32)(a > 29491u ? 29491u : a);
    }
}

/* ---- delay ---- */

static inline s32 delay_read(u32 side)
{
    u32 q8 = delay_q8[side], back = q8 >> 8, f = q8 & 255u;
    s32 a = delay_ram[side][(delay_write - back) & DELAY_MASK];
    s32 b = delay_ram[side][(delay_write - back - 1u) & DELAY_MASK];
    return a + (((b - a) * (s32)f) >> 8);
}

static inline s32 soft_clip(s32 v) /* Q15 in, Q15 out within +-1 */
{
    if (v > SOFT_CLIP_LIMIT) v = SOFT_CLIP_LIMIT;
    if (v < -SOFT_CLIP_LIMIT) v = -SOFT_CLIP_LIMIT;
    s64 cube = (s64)v * v * v;                /* Q45 */
    return v - (s32)(cube * 4 / ((s64)27 << 30)); /* v - v^3 / 6.75 */
}

/* The delay's beat in Q8 frames from its Tempo: MIDI clock and channels
 * through Time Lock's clocks, else the set BPM. */
static void drives_foreground(void);
void mix_fx_foreground(void)
{
    /* A preset load or a channel copy changed the settings. */
    static u32 loaded, copies;
    if (loaded != patch_grain_loaded || copies != records_copied_count) {
        loaded = patch_grain_loaded; copies = records_copied_count;
        mix_fx_update();
    }
    drives_foreground();
    u32 tempo = mix_now()->delay_tempo;
    if (tempo >= MIX_TEMPO_BPM) { beat_q8 = 0; return; }
    beat_q8 = timelock_beat_q8(tempo == MIX_TEMPO_MIDI ? PRESET_CHANNELS : tempo - MIX_TEMPO_CHANNEL);
}
/* Drive curves for the Drive settings, built here and used by the
 * interrupt once marked built. */
static void drives_foreground(void)
{
    for (u32 c = 0; c <= MIX_CHANNELS; ++c) {
        u32 amount = c < MIX_CHANNELS ? mix_now()->comp[c].drive : mix_now()->master.drive;
        if (!amount || drive_built[c] == amount + 1u) continue;
        drive_built[c] = 0;
        MEMORY_BARRIER();
        drive_build(drive_curve[c], amount);
        MEMORY_BARRIER();
        drive_built[c] = (u8)(amount + 1u);
    }
}

u32 mix_fx_reduction_db(u32 compressor)
{
    s32 gr = state[compressor <= MIX_CHANNELS ? compressor : MIX_CHANNELS].gr;
    return gr > 0 ? (u32)(((s64)gr * 60206 / 10000 + (1 << 23)) >> 24) : 0;
}

int mix_fx_wants_midi_clock(void) { return mix_now()->delay_tempo == MIX_TEMPO_MIDI; }

static u32 set_bpm(void)
{
    u32 tempo = mix_now()->delay_tempo;
    return tempo >= MIX_TEMPO_BPM ? MIX_BPM_MIN + tempo - MIX_TEMPO_BPM : 0;
}

u32 mix_fx_bpm10(void)
{
    u32 beat = beat_q8, bpm = set_bpm();
    if (bpm) return bpm * 10u;
    return beat ? (u32)((u64)FRAME_HZ * 600u * 256u / beat) : 0;
}

static u32 division_now;  /* the delay's Time after its CV */
static void delay_target(void)
{
    /* A stopped clock keeps the last time; no time yet is 120 BPM. */
    u32 bpm = set_bpm(), beat = bpm ? FRAME_HZ * 60u / bpm : beat_q8 >> 8;
    if (!beat) { if (delay_target_q8) return; beat = FRAME_HZ * 60u / 120u; }
    u32 d = division_now < MIX_DIVISIONS ? division_now : 0;
    u32 frames_d = beat * division_units[d] / DIVISION_UNITS;
    while (frames_d > DELAY_WORDS - 4u) frames_d >>= 1;
    if (frames_d < DELAY_MIN_FRAMES) frames_d = DELAY_MIN_FRAMES;
    delay_target_q8 = frames_d << 8;
}

/* ---- settings and CV ---- */

#define CONTROL_MASK       31u    /* the routed mix settings follow CV every 32 frames */
#define GLIDE_SHIFT        6      /* gains follow a change over about 1.3 ms */
#define CV_FULL_SCALE      32768  /* 5 V */
#define AMOUNT_FULL        100
static volatile u32 dirty;
static u32 comps_pending;          /* compressors whose coefficients are due, one a frame */
static s32 send_input[MIX_CHANNELS][MIX_SEND_ROUTES], bus_input[MIX_BUS_ROUTES]; /* CV inputs, -1: none */
static s32 send_now[MIX_CHANNELS][MIX_SEND_ROUTES];  /* Q15, gliding */
static s32 reverb_level_target, delay_level_target, feedback_target;
static u32 decay_now, damp_now;

void mix_fx_update(void) { dirty = 1; }

static s32 clamp32(s32 v, s32 low, s32 high) { return v < low ? low : v > high ? high : v; }
/* A setting moved by its route: at +100, 5 V crosses the whole range. */
static s32 routed(u32 base, const struct mix_route *r, s32 input, s32 low, s32 high)
{
    s32 v = (s32)base;
    if (input >= 0 && r->amount)
        v += r->amount * ((volatile signed short *)EQ_CV_SAMPLES)[input] / AMOUNT_FULL * (high - low) / CV_FULL_SCALE;
    return clamp32(v, low, high);
}
static inline s32 glide(s32 at, s32 target) { return at + ((target - at) >> GLIDE_SHIFT); }

/* The routes' inputs and the compressors, after an edit. */
static void settings_changed(void)
{
    for (u32 c = 0; c < MIX_CHANNELS; ++c)
        for (u32 n = 0; n < MIX_SEND_ROUTES; ++n)
            send_input[c][n] = mix_now()->send_route[c][n].amount ? eq_cv_input(mix_now()->send_route[c][n].source, c) : -1;
    for (u32 n = 0; n < MIX_BUS_ROUTES; ++n)
        bus_input[n] = mix_now()->bus_route[n].amount ? eq_cv_input(mix_now()->bus_route[n].source, 0) : -1;
    comps_pending = (1u << (MIX_CHANNELS + 1)) - 1u;
}

/* The mix settings as their routes move them now. */
static u8 pair_right[MIX_CHANNELS];   /* a channel in Stereo/Right mode */
static void control(void)
{
    const struct mix_settings *m = mix_now();
    for (u32 c = 1; c < MIX_CHANNELS; ++c) pair_right[c] = channel_record(c)[CHANNEL_MODE] == CHANNEL_MODE_STEREO_RIGHT;
    const struct mix_route *r = m->bus_route;
    u32 decay = (u32)routed(m->reverb_decay, &r[MIX_ROUTE_REVERB_DECAY], bus_input[MIX_ROUTE_REVERB_DECAY], MIX_DECAY_MIN, MIX_DECAY_MAX);
    u32 damp = (u32)routed(m->reverb_damp, &r[MIX_ROUTE_REVERB_DAMP], bus_input[MIX_ROUTE_REVERB_DAMP], 0, MIX_PERCENT_MAX);
    if (decay != decay_now || damp != damp_now) { decay_now = decay; damp_now = damp; reverb_update(decay, damp); }
    reverb_level_target = routed(m->reverb_level, &r[MIX_ROUTE_REVERB_LEVEL], bus_input[MIX_ROUTE_REVERB_LEVEL], 0, MIX_PERCENT_MAX) * PERCENT_Q15;
    delay_level_target = routed(m->delay_level, &r[MIX_ROUTE_DELAY_LEVEL], bus_input[MIX_ROUTE_DELAY_LEVEL], 0, MIX_PERCENT_MAX) * PERCENT_Q15;
    feedback_target = routed(m->delay_feedback, &r[MIX_ROUTE_DELAY_FEEDBACK], bus_input[MIX_ROUTE_DELAY_FEEDBACK], 0, MIX_FEEDBACK_MAX) * PERCENT_Q15;
    u32 low = (u32)routed(m->delay_low, &r[MIX_ROUTE_DELAY_LOW], bus_input[MIX_ROUTE_DELAY_LOW], 0, MIX_FILTER_STEPS - 1);
    u32 high = (u32)routed(m->delay_high, &r[MIX_ROUTE_DELAY_HIGH], bus_input[MIX_ROUTE_DELAY_HIGH], 0, MIX_FILTER_STEPS - 1);
    low_coef = mix_low_cut_coef[low];
    high_coef = mix_high_cut_coef[high];
    u32 rate = (u32)routed(m->chorus_rate, &r[MIX_ROUTE_CHORUS_RATE], bus_input[MIX_ROUTE_CHORUS_RATE], 0, MIX_PERCENT_MAX);
    u32 depth = (u32)routed(m->chorus_depth, &r[MIX_ROUTE_CHORUS_DEPTH], bus_input[MIX_ROUTE_CHORUS_DEPTH], 0, MIX_PERCENT_MAX);
    chorus_step = (u32)(((u64)CHORUS_RATE_LOW * (u32)exp2_q24((s32)((s64)CHORUS_RATE_OCTAVES * rate / MIX_PERCENT_MAX))) >> 24);
    chorus_depth_q8 = depth * CHORUS_DEPTH_MAX * 256u / MIX_PERCENT_MAX;
    gated_scale_q8 = decay * GATE_MS_PER_TENTH * 256u / GATE_BASE_MS;
    u32 division = (u32)routed(m->delay_division, &r[MIX_ROUTE_DELAY_TIME], bus_input[MIX_ROUTE_DELAY_TIME], 0, MIX_DIVISIONS - 1);
    if (division != division_now) { division_now = division; delay_target(); }
}

/* ---- the hooks ---- */

/* A channel's drive, compressor and mix; a Stereo/Right pair shares its
 * master's settings and one detector. */
static void compress_channels(s32 *ch, u32 c, u32 pair)
{
    const struct comp_coef *k = &coef[c];
    if (!k->on && !k->drive) return;
    u32 n = pair ? 2u : 1u, driven = k->drive && drive_built[c] == k->drive + 1u;
    s32 x[2];
    for (u32 i = 0; i < n; ++i) x[i] = driven ? drive(drive_curve[c], ch[c + i]) : ch[c + i];
    s32 g = GAIN_ONE_Q24;
    if (k->on) {
        u32 key = mix_now()->comp[c].key;
        s32 detect = x[0];
        if (key && key <= MIX_KEY_INPUT) detect = keys[key];
        else if (pair) {
            s32 a = x[0] < 0 ? -(x[0] + 1) : x[0], b = x[1] < 0 ? -(x[1] + 1) : x[1];
            detect = a > b ? a : b;
        }
        g = comp_gain(k, &state[c], detect);
    } else state[c].gr = 0;
    for (u32 i = 0; i < n; ++i) ch[c + i] = blend(ch[c + i], apply(x[i], g), k->mix_q15);
}

static void channel_frame(s32 *ch)
{
    for (u32 c = 0; c < MIX_CHANNELS; ++c) keys[1 + c] = ch[c];
    /* The Sample input as a sidechain: the larger side. */
    s32 l = patch_input_frame[0], r = patch_input_frame[1];
    l = l < 0 ? -(l + 1) : l; r = r < 0 ? -(r + 1) : r;
    keys[MIX_KEY_INPUT] = l > r ? l : r;
    for (u32 c = 0; c < MIX_CHANNELS; ++c) {
        if (pair_right[c]) continue;
        u32 pair = c + 1 < MIX_CHANNELS && pair_right[c + 1];
        compress_channels(ch, c, pair);
        const struct mix_route *route = mix_now()->send_route[c];
        s32 *now = send_now[c];
        now[MIX_ROUTE_REVERB_SEND] = glide(now[MIX_ROUTE_REVERB_SEND], routed(mix_now()->reverb_send[c], &route[MIX_ROUTE_REVERB_SEND],
            send_input[c][MIX_ROUTE_REVERB_SEND], 0, MIX_PERCENT_MAX) * PERCENT_Q15);
        now[MIX_ROUTE_DELAY_SEND] = glide(now[MIX_ROUTE_DELAY_SEND], routed(mix_now()->delay_send[c], &route[MIX_ROUTE_DELAY_SEND],
            send_input[c][MIX_ROUTE_DELAY_SEND], 0, MIX_PERCENT_MAX) * PERCENT_Q15);
    }
}

/* The sends, from the mix hook: each channel before its level (Pre) or
 * after its level, mute and solo (Post). A pair's right side takes its
 * master's sends. */
static void sends_frame(void)
{
    const struct mix_settings *m = mix_now();
    s64 rev = 0, dly = 0;
    for (u32 c = 0; c < MIX_CHANNELS; ++c) {
        if (pair_right[c]) continue;
        const s32 *from = m->send_pre[c] ? patch_channel_out : patch_fader_out;
        s64 in = (s64)from[c] + (c + 1 < MIX_CHANNELS && pair_right[c + 1] ? from[c + 1] : 0);
        rev += in * send_now[c][MIX_ROUTE_REVERB_SEND];
        dly += in * send_now[c][MIX_ROUTE_DELAY_SEND];
    }
    send_reverb = sat32(rev >> 15);
    send_delay = sat32(dly >> 15);
}

static void mix_frame(s32 *frame)
{
    s32 mix[2] = {frame[2] - frame[0], frame[3] - frame[1]};
    ++frames;
    sends_frame();
    if (dirty) { dirty = 0; settings_changed(); control(); }
    else if (!(frames & CONTROL_MASK)) control();
    if (comps_pending) {
        u32 c = (u32)__builtin_ctz(comps_pending);
        comps_pending &= comps_pending - 1u;
        comp_coef_set(&coef[c], c < MIX_CHANNELS ? &mix_now()->comp[c] : &mix_now()->master);
    }
    if (!(frames & DELAY_RETARGET_MASK)) delay_target();
    reverb_level_q15 = glide(reverb_level_q15, reverb_level_target);
    delay_level_q15 = glide(delay_level_q15, delay_level_target);
    feedback_q15 = glide(feedback_q15, feedback_target);

    /* Reverb at 32 kHz: two ticks every three frames. */
    for (u32 i = MIX_DOWN_TAPS - 1; i; --i) down_hist[i] = down_hist[i - 1];
    down_hist[0] = send_reverb >> 16;
    if (phase != 2) {
        s32 acc = 0;
        for (u32 i = 0; i < MIX_DOWN_TAPS; ++i) acc += down_hist[i] * mix_down_fir[phase][i];
        s32 l, r;
        reverb_tick(sat16(acc >> 15), &l, &r);
        for (u32 i = MIX_UP_TAPS - 1; i; --i) { up_hist[0][i] = up_hist[0][i - 1]; up_hist[1][i] = up_hist[1][i - 1]; }
        up_hist[0][0] = l; up_hist[1][0] = r;
    }
    s32 wet[2] = {0, 0};
    for (u32 i = 0; i < MIX_UP_TAPS; ++i) { wet[0] += up_hist[0][i] * mix_up_fir[phase][i]; wet[1] += up_hist[1][i] * mix_up_fir[phase][i]; }
    phase = phase == 2 ? 0 : phase + 1;

    /* Ping-pong delay: the send enters the left line, each line feeds the other. */
    u32 target = delay_target_q8;
    for (u32 s = 0; s < 2; ++s) delay_q8[s] += (u32)((s32)(target - delay_q8[s]) >> DELAY_GLIDE_SHIFT);
    /* Each repeat, the first too, passes the low and high cut. */
    s32 echo[2];
    for (u32 s = 0; s < 2; ++s) {
        s32 v = delay_read(s);
        high_state[s] += ((v - high_state[s]) * high_coef) >> 15;
        v = high_state[s];
        low_state[s] += ((v - low_state[s]) * low_coef) >> 15;
        echo[s] = v - low_state[s];
    }
    s32 into_left = (send_delay >> 17) + ((echo[1] * feedback_q15) >> 15);
    s32 into_right = (echo[0] * feedback_q15) >> 15;
    delay_ram[0][delay_write & DELAY_MASK] = (s16)soft_clip(into_left);
    delay_ram[1][delay_write & DELAY_MASK] = (s16)soft_clip(into_right);
    delay_write = (delay_write + 1u) & DELAY_MASK;

    for (u32 s = 0; s < 2; ++s) {
        /* Q15 returns times Q15 levels, to Q31; the delay's send has 6 dB of headroom to restore. */
        s64 add = ((s64)(wet[s] >> 15) * reverb_level_q15 << 1) + ((s64)echo[s] * delay_level_q15 << 2);
        mix[s] = sat32((s64)mix[s] + add);
    }
    const struct comp_coef *k = &coef[MIX_CHANNELS];
    if (k->on || k->drive) {
        u32 driven = k->drive && drive_built[MIX_CHANNELS] == k->drive + 1u;
        s32 x[2] = {mix[0], mix[1]};
        if (driven) { x[0] = drive(drive_curve[MIX_CHANNELS], x[0]); x[1] = drive(drive_curve[MIX_CHANNELS], x[1]); }
        s32 g = GAIN_ONE_Q24;
        if (k->on) {
            /* Both sides share one detector. */
            u32 source = mix_now()->master.key;
            s32 a = x[0] < 0 ? -(x[0] + 1) : x[0], b = x[1] < 0 ? -(x[1] + 1) : x[1];
            s32 key = source && source <= MIX_KEY_INPUT ? keys[source] : a > b ? a : b;
            g = comp_gain(k, &state[MIX_CHANNELS], key);
        } else state[MIX_CHANNELS].gr = 0;
        mix[0] = blend(mix[0], apply(x[0], g), k->mix_q15);
        mix[1] = blend(mix[1], apply(x[1], g), k->mix_q15);
    }
    frame[0] = sat32((s64)frame[2] - mix[0]);
    frame[1] = sat32((s64)frame[3] - mix[1]);
}

void mix_fx_init(void)
{
    static const struct mix_comp_set comp_default = {0, 20, 4, 3, 0, 0, MIX_PERCENT_MAX, MIX_KEY_SELF};
    mix_defaults.format = MIX_FORMAT;
    for (u32 c = 0; c < MIX_CHANNELS; ++c) mix_defaults.comp[c] = comp_default;
    mix_defaults.master = comp_default;
    mix_defaults.reverb_decay = 25; mix_defaults.reverb_damp = 20; mix_defaults.reverb_level = 80;
    mix_defaults.delay_division = 5; mix_defaults.delay_feedback = 45; mix_defaults.delay_low = 3;
    mix_defaults.delay_high = 2; mix_defaults.delay_level = 80; mix_defaults.delay_tempo = MIX_TEMPO_MIDI;
    mix_defaults.reverb_algo = 1; mix_defaults.chorus_rate = 40; mix_defaults.chorus_depth = 50;
    for (u32 n = 0; n < MIX_BUS_ROUTES; ++n) mix_defaults.bus_route[n].source = EQ_CV_SOURCE_OFF;
    for (u32 c = 0; c < MIX_CHANNELS; ++c)
        for (u32 n = 0; n < MIX_SEND_ROUTES; ++n) mix_defaults.send_route[c][n].source = EQ_CV_SOURCE_OFF;
    reverb_layout();
    reverb_constants();
    settings_changed();
    while (comps_pending) {
        u32 c = (u32)__builtin_ctz(comps_pending);
        comps_pending &= comps_pending - 1u;
        comp_coef_set(&coef[c], c < MIX_CHANNELS ? &mix_now()->comp[c] : &mix_now()->master);
    }
    division_now = MIX_DIVISIONS; decay_now = 0;
    control();
    reverb_level_q15 = reverb_level_target; delay_level_q15 = delay_level_target; feedback_q15 = feedback_target;
    delay_q8[0] = delay_q8[1] = delay_target_q8;
    MEMORY_BARRIER();
    patch_channel_fx_hook = channel_frame;
    patch_mix_fx_hook = mix_frame;
}
