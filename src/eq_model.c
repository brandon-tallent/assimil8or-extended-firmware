#include "eq.h"
/* The EQ settings are a preset's (preset_extras.c).
 *
 * The foreground publishes complete coefficient banks. It rebuilds only the
 * lanes whose effective (CV-modulated) settings changed, samples CV at most
 * every EQ_CV_UPDATE_TICKS, and waits while a bank is pending, so continuous
 * modulation costs a bounded amount of foreground time. The renderer's glide
 * smooths each step. */
/* Calibrated CV inputs: 24 signed halfwords, three rows (A/B/C) of eight
 * (ENGINE_CALIBRATION_CV_SAMPLE_BASE); 32768 is 5 V. */
#define EQ_CV_SAMPLES 0x9ffffe50u
#define EQ_CV_INPUTS 24u
#define EQ_CV_ROW_WIDTH 8u
#define EQ_CV_BANK_WIDTH 3u
#define EQ_CV_VOLTS_PER_COUNT (5.0f/32768)
#define EQ_CV_DEADBAND 48            /* counts, about 7 mV: above idle input noise */
#define EQ_CV_UPDATE_TICKS 2u        /* ~2 ms between CV-driven banks */
static struct eq_value base_used[8], rendered[8];
static signed short cv_held[EQ_CV_INPUTS];
static struct eq_bank banks[2];
struct eq_mailbox_state eq_mailbox;
s32 eq_steps[2][5][5][4];
static u32 initialized, force, cv_moved, cv_sampled_at;
void eq_default(struct eq_value *value)
{
    memset(value, 0, sizeof(*value));
    value->frequency[0] = EQ_STEP_150HZ;
    value->frequency[1] = EQ_STEP_1K;
    value->frequency[2] = EQ_STEP_10K;
    for (u32 m = 0; m < EQ_MOD_COUNT; ++m) {
        value->source[m] = EQ_CV_SOURCE_OFF;
        value->amount[m] = EQ_CV_AMOUNT_MAX;
    }
}
/* Other settings were loaded: publish every lane again. */
void eq_changed(void) { initialized = force = 1; }
s32 eq_cv_input(s32 source, u32 channel)
{
    if (source < EQ_CV_SOURCE_RELATIVE_FIRST || source > EQ_CV_SOURCE_MAX) return -1;
    return source < 0 ? (s32)((u32)(source - EQ_CV_SOURCE_RELATIVE_FIRST)*EQ_CV_ROW_WIDTH + channel)
        : (s32)((u32)source%EQ_CV_BANK_WIDTH*EQ_CV_ROW_WIDTH + (u32)source/EQ_CV_BANK_WIDTH);
}
s32 eq_cv_counts(s32 source, u32 channel)
{
    s32 input = eq_cv_input(source, channel);
    return input < 0 ? 0 : cv_held[input];
}
static float volts(s32 source, u32 channel) { return eq_cv_counts(source, channel)*EQ_CV_VOLTS_PER_COUNT; }
u32 eq_modulated(const struct eq_value *value)
{
    for (u32 m = 0; m < EQ_MOD_COUNT; ++m)
        if (value->source[m] != EQ_CV_SOURCE_OFF && value->amount[m]) return 1;
    return 0;
}
void eq_modulate(const struct eq_value *value, u32 channel, struct eq_value *out)
{
    *out = *value;
    for (u32 m = 0; m < EQ_MOD_COUNT; ++m) {
        if (value->source[m] == EQ_CV_SOURCE_OFF || !value->amount[m]) continue;
        float amount = value->amount[m]*(1.0f/EQ_CV_AMOUNT_MAX)*volts(value->source[m], channel);
        u32 band = m/2;
        if (m == EQ_MOD_HPF) {
            if (value->hpf) out->hpf = eq_frequency_shift(value->hpf, amount, EQ_HPF_MIN, EQ_HPF_MAX);
        } else if (m%2) {
            s32 gain = value->gain[band] + (s32)(amount*EQ_CV_GAIN_TENTHS_PER_VOLT + (amount < 0 ? -0.5f : 0.5f));
            s32 limit = eq_gain_limit(value, band);
            out->gain[band] = (signed short)(gain < -limit ? -limit : gain > limit ? limit : gain);
        } else {
            s32 step = value->frequency[band] + (s32)(amount*EQ_STEPS_PER_OCTAVE + (amount < 0 ? -0.5f : 0.5f));
            out->frequency[band] = (u16)(step < 0 ? 0 : step > EQ_STEP_MAX ? EQ_STEP_MAX : step);
        }
    }
}
void eq_automate(const u8 *p, u32 bytes)
{
    if (bytes != EQ_AUTOMATE_BYTES || p[0] >= 8 || p[1] >= EQ_AUTOMATE_FIELDS) return;
    u32 field = p[1], value = (p[2] & 15u) | (p[3] & 15u) << 4 | (p[4] & 15u) << 8 | (p[5] & 15u) << 12;
    struct eq_value next = eq_channels[p[0]];
    if (field < EQ_AUTOMATE_GAIN)
        next.frequency[field] = (u16)eq_step_above_min(value);
    else if (field < EQ_AUTOMATE_Q) {
        if (value > 2*EQ_GAIN_MAX/EQ_TENTHS_PER_DB) value = 2*EQ_GAIN_MAX/EQ_TENTHS_PER_DB;
        next.gain[field - EQ_AUTOMATE_GAIN] = (signed short)((s32)value*EQ_TENTHS_PER_DB - EQ_GAIN_MAX);
    } else if (field < EQ_AUTOMATE_HPF) next.q[field - EQ_AUTOMATE_Q] = (u8)(value > EQ_Q_STEPS ? EQ_Q_STEPS : value);
    else if (field == EQ_AUTOMATE_HPF) next.hpf = (u16)(!value ? 0 : value < EQ_HPF_MIN ? EQ_HPF_MIN : value > EQ_HPF_MAX ? EQ_HPF_MAX : value);
    else next.on = value != 0;
    eq_limit(&next);
    eq_channels[p[0]] = next;
}
static void sample_cv(void)
{
    u32 now = REG32(TICKS);
    if ((u32)(now - cv_sampled_at) < EQ_CV_UPDATE_TICKS) return;
    cv_sampled_at = now;
    for (u32 n = 0; n < EQ_CV_INPUTS; ++n) {
        s32 sample = ((volatile signed short *)EQ_CV_SAMPLES)[n], delta = sample - cv_held[n];
        if (delta >= EQ_CV_DEADBAND || delta <= -EQ_CV_DEADBAND) { cv_held[n] = (signed short)sample; cv_moved = 1; }
    }
}
static void lane(struct eq_bank *bank, u32 ch, const struct eq_value *v)
{
    s32 row[5][5];
    eq_coefficients(v, row);
    bank->group[ch/4].mask[ch%4] = v->on && (v->hpf || v->gain[0] || v->gain[1] || v->gain[2]) ? -1 : 0;
    for (u32 section = 0; section < 5; ++section)
        for (u32 c = 0; c < 5; ++c) bank->group[ch/4].coefficient[section][c][ch%4] = row[section][c];
}
void eq_foreground_poll(void)
{
    if (!initialized) preset_extras_reset();
    if (eq_mailbox.pending) return;
    sample_cv();
    struct eq_bank *bank = eq_mailbox.active == &banks[0] ? &banks[1] : &banks[0];
    u32 changed = force || !eq_mailbox.active;
    /* Start from the published set; only changed lanes are rebuilt. */
    if (!changed) memcpy(bank, eq_mailbox.active, sizeof(*bank));
    for (u32 ch = 0; ch < 8; ++ch) {
        u32 master = ch;
        if (ch && BYTE(EQ_CHANNEL_RECORDS + ch*EQ_CHANNEL_BYTES + EQ_CHANNEL_MODE) == EQ_CHANNEL_MODE_STEREO_RIGHT) master--;
        u32 rebuild = changed;
        if (master != ch) {
            /* Right follows its master's snapshot so a panel interrupt between
             * lanes cannot briefly publish different stereo coefficients. */
            memset(&base_used[ch], ~0, sizeof(base_used[ch])); /* rebuild if it becomes independent */
            if (rebuild || memcmp(&rendered[ch], &rendered[master], sizeof(rendered[ch]))) {
                rendered[ch] = rendered[master];
                changed = 1;
                lane(bank, ch, &rendered[ch]);
            }
            continue;
        }
        u32 zone = REG32(EQ_ZONE_SELECTOR_BASE + master*4) & EQ_ZONE_MASK;
        const struct eq_value *base = eq_zones[master][zone].override ? &eq_zones[master][zone] : &eq_channels[master];
        if (!rebuild && !memcmp(base, &base_used[ch], sizeof(*base)) && !(cv_moved && eq_modulated(base))) continue;
        base_used[ch] = *base;
        struct eq_value wanted;
        eq_modulate(base, master, &wanted);
        wanted.override = 0;
        if (!rebuild && !memcmp(&wanted, &rendered[ch], sizeof(wanted))) continue;
        rendered[ch] = wanted;
        changed = 1;
        lane(bank, ch, &wanted);
    }
    cv_moved = 0;
    if (!changed) return;
    force = 0;
    MEMORY_BARRIER();
    eq_mailbox.pending = bank;
}

/* The channel filter's settings are a preset's, as the EQ's. The foreground
 * publishes a complete bank of lane vectors and rebuilds only the lanes whose
 * effective settings changed; the renderer closes on each change by itself
 * (filter.S), so there is no glide step here. Resonance, Extra and Type
 * follow their CV here, as the EQ's routes do; the renderer moves the cutoff
 * by its CV each block, as stock moves pitch. A lane turned Off keeps the
 * vectors of its last setting, with the mask cleared, so the renderer can
 * fade it out through them. */
struct filter_bank { s32 vector[2][FILTER_VECTORS][4]; } __attribute__((aligned(16)));
struct filter_mailbox_state { struct filter_bank *active, *pending; };
struct filter_mailbox_state filter_mailbox;
s32 filter_current[2][21][4] __attribute__((aligned(16)));
s32 filter_state[2][6][4] __attribute__((aligned(16)));
static struct filter_bank filter_banks[2];
static struct filter_value filter_base_used[8], filter_rendered[8], filter_last_on[8];
static u32 filter_force = 1;
#define CV_INPUT_BYTES 2u /* a calibrated input's halfword */
void filter_default(struct filter_value *value, u32 channel)
{
    (void)channel;
    memset(value, 0, sizeof(*value));
    value->cutoff = EQ_STEP_1K;
    value->extra = 50;
    for (u32 m = 0; m < FILTER_MOD_COUNT; ++m) {
        value->source[m] = EQ_CV_SOURCE_OFF;
        value->amount[m] = EQ_CV_AMOUNT_MAX;
    }
}
void filter_changed(void) { filter_force = 1; }
static u32 routed(const struct filter_value *value, u32 m)
{
    return value->source[m] != EQ_CV_SOURCE_OFF && value->amount[m];
}
u32 filter_modulated(const struct filter_value *value)
{
    for (u32 m = 0; m < FILTER_MOD_COUNT; ++m)
        if (routed(value, m)) return 1;
    return 0;
}
static s32 steps(float volts_amount, s32 per_volt)
{
    float x = volts_amount*per_volt;
    return (s32)(x + (x < 0 ? -0.5f : 0.5f));
}
static s32 clamped(s32 x, s32 low, s32 high) { return x < low ? low : x > high ? high : x; }
/* Type CV moves to the next type only once it is this far past the half-way
 * point, so noise near a boundary does not flip the type back and forth. */
#define FILTER_TYPE_HYSTERESIS 0.25f
static u8 type_held[8];
/* The type a Type route chooses for channel: the type nearest the CV, kept
 * until the CV is clearly nearer another. `keep` records it (the
 * foreground); the page's graph only reads. */
static u32 routed_type(const struct filter_value *value, u32 channel, float amount, u32 keep)
{
    float position = value->type + amount*FILTER_TYPE_STEPS_PER_VOLT;
    s32 held = type_held[channel];
    float distance = position - (float)held;
    if (held < FILTER_OFF + 1 || held > FILTER_TYPES - 1 || distance > 0.5f + FILTER_TYPE_HYSTERESIS ||
        distance < -0.5f - FILTER_TYPE_HYSTERESIS)
        held = (s32)(position + (position < 0 ? -0.5f : 0.5f));
    held = clamped(held, FILTER_OFF + 1, FILTER_TYPES - 1);
    if (keep) type_held[channel] = (u8)held;
    return (u32)held;
}
/* Every route but the cutoff's, which the renderer applies. */
static void filter_modulate(const struct filter_value *value, u32 channel, struct filter_value *out, u32 keep)
{
    *out = *value;
    for (u32 m = FILTER_MOD_RESONANCE; m < FILTER_MOD_COUNT; ++m) {
        if (!routed(value, m)) continue;
        float amount = value->amount[m]*(1.0f/EQ_CV_AMOUNT_MAX)*volts(value->source[m], channel);
        if (m == FILTER_MOD_TYPE) {
            /* Type moves through the filter types; an Off filter stays off. */
            if (value->type != FILTER_OFF) out->type = (u8)routed_type(value, channel, amount, keep);
        } else if (m == FILTER_MOD_RESONANCE)
            out->resonance = (u8)clamped(value->resonance + steps(amount, FILTER_STEPS_PER_VOLT), 0, FILTER_RESONANCE_MAX);
        else out->extra = (u8)clamped(value->extra + steps(amount, FILTER_STEPS_PER_VOLT), 0, FILTER_EXTRA_MAX);
    }
}
void filter_live(const struct filter_value *value, u32 channel, struct filter_value *out)
{
    filter_modulate(value, channel, out, 0);
    if (!routed(value, FILTER_MOD_CUTOFF)) return;
    float octaves = value->amount[FILTER_MOD_CUTOFF]*(1.0f/EQ_CV_AMOUNT_MAX)*FILTER_CUTOFF_OCTAVES_PER_VOLT*
        volts(value->source[FILTER_MOD_CUTOFF], channel);
    out->cutoff = (u16)clamped(value->cutoff + steps(octaves, EQ_STEPS_PER_OCTAVE), 0, EQ_STEP_MAX);
}
/* channel's inputs are the ones its cutoff CV reads (a Stereo/Right lane's
 * are its master's). */
static void filter_store(struct filter_bank *bank, u32 ch, u32 channel, const struct filter_value *v)
{
    s32 row[FILTER_VECTORS];
    if (v->type != FILTER_OFF) filter_last_on[ch] = *v;
    const struct filter_value *used = v->type != FILTER_OFF || !filter_last_on[ch].type ? v : &filter_last_on[ch];
    filter_lane(used, row);
    if (v->type == FILTER_OFF) row[FILTER_VECTOR_MASK] = 0;
    s32 input = routed(used, FILTER_MOD_CUTOFF) ? eq_cv_input(used->source[FILTER_MOD_CUTOFF], channel) : -1;
    if (input >= 0) {
        union { float f; s32 bits; } scale = { .f = used->amount[FILTER_MOD_CUTOFF]*(1.0f/EQ_CV_AMOUNT_MAX)*
            FILTER_CUTOFF_OCTAVES_PER_VOLT*EQ_CV_VOLTS_PER_COUNT };
        row[FILTER_VECTOR_CV_INPUT] = input*(s32)CV_INPUT_BYTES;
        row[FILTER_VECTOR_CV_SCALE] = scale.bits;
        row[FILTER_VECTOR_DYNAMIC_MASK] = -1;
    }
    s32 (*group)[4] = bank->vector[ch/4];
    for (u32 n = 0; n < FILTER_VECTORS; ++n) group[n][ch%4] = row[n];
    /* The renderer skips the moving cutoff when no lane of the group uses it. */
    u32 moves = 0;
    for (u32 lane = 0; lane < 4; ++lane) moves |= group[FILTER_VECTOR_DYNAMIC_MASK][lane];
    group[FILTER_VECTOR_FLAGS][FILTER_FLAG_DYNAMIC] = moves != 0;
}
void filter_foreground_poll(void)
{
    if (filter_mailbox.pending) return;
    struct filter_bank *bank = filter_mailbox.active == &filter_banks[0] ? &filter_banks[1] : &filter_banks[0];
    u32 rebuild = filter_force || !filter_mailbox.active, changed = rebuild;
    if (!rebuild) memcpy(bank, filter_mailbox.active, sizeof(*bank));
    for (u32 ch = 0; ch < 8; ++ch) {
        u32 master = ch;
        if (ch && BYTE(EQ_CHANNEL_RECORDS + ch*EQ_CHANNEL_BYTES + EQ_CHANNEL_MODE) == EQ_CHANNEL_MODE_STEREO_RIGHT) master--;
        if (master != ch) {
            /* The master's settings, with the cutoff moved by the master's
             * right-side offset. Cutoff CV then moves both lanes. */
            struct filter_value right = filter_rendered[master];
            right.cutoff = (u16)filter_right_cutoff(&right);
            right.right = 0;
            memset(&filter_base_used[ch], ~0, sizeof(filter_base_used[ch]));
            if (rebuild || memcmp(&filter_rendered[ch], &right, sizeof(right))) {
                filter_rendered[ch] = right;
                changed = 1;
                filter_store(bank, ch, master, &filter_rendered[ch]);
            }
            continue;
        }
        u32 zone = REG32(EQ_ZONE_SELECTOR_BASE + master*4) & EQ_ZONE_MASK;
        const struct filter_value *base = filter_zones[master][zone].override ? &filter_zones[master][zone] : &filter_channels[master];
        if (!rebuild && !memcmp(base, &filter_base_used[ch], sizeof(*base)) && !filter_modulated(base)) continue;
        filter_base_used[ch] = *base;
        struct filter_value wanted;
        filter_modulate(base, master, &wanted, 1);
        wanted.override = 0;
        if (!rebuild && !memcmp(&wanted, &filter_rendered[ch], sizeof(wanted))) continue;
        filter_rendered[ch] = wanted;
        changed = 1;
        filter_store(bank, ch, master, &wanted);
    }
    if (!changed) return;
    filter_force = 0;
    MEMORY_BARRIER();
    filter_mailbox.pending = bank;
}
