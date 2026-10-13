#include "eq.h"
/* The EQ settings are a preset's (preset_extras.c).
 *
 * The foreground publishes complete coefficient banks. It rebuilds only the
 * lanes whose effective (CV-modulated) settings changed, samples CV at most
 * every EQ_CV_UPDATE_TICKS, and waits while a bank is pending, so continuous
 * modulation costs a bounded amount of foreground time. The renderer's glide
 * smooths each step. */
#define EQ_ZONE_SELECTOR_BASE 0x9ffff580u  /* ENGINE_CHANNEL_ZONE_SELECTOR_BASE */
#define EQ_CHANNEL_RECORDS (EQ_MODEL + 0x20u) /* CURRENT_WORKING_PRESET_FIRST_CHANNEL */
#define EQ_CHANNEL_MODE 0x264u              /* PRESET_CHANNEL_MODE */
#define EQ_CHANNEL_MODE_STEREO_RIGHT 2u
#define EQ_ZONE_MASK 7u
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
    value->on = 1;
    value->frequency[0] = 150;
    value->frequency[1] = 1000;
    value->frequency[2] = 10000;
    for (u32 m = 0; m < EQ_MOD_COUNT; ++m) {
        value->source[m] = EQ_CV_SOURCE_OFF;
        value->amount[m] = EQ_CV_AMOUNT_MAX;
    }
}
/* Other settings were loaded: publish every lane again. */
void eq_changed(void) { initialized = force = 1; }
s32 eq_cv_counts(s32 source, u32 channel)
{
    if (source < EQ_CV_SOURCE_RELATIVE_FIRST || source > EQ_CV_SOURCE_MAX) return 0;
    u32 input = source < 0 ? (u32)(source - EQ_CV_SOURCE_RELATIVE_FIRST)*EQ_CV_ROW_WIDTH + channel
        : (u32)source%EQ_CV_BANK_WIDTH*EQ_CV_ROW_WIDTH + (u32)source/EQ_CV_BANK_WIDTH;
    return cv_held[input];
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
        } else
            out->frequency[band] = eq_frequency_shift(value->frequency[band], amount, EQ_FREQUENCY_MIN, EQ_FREQUENCY_MAX);
    }
}
void eq_automate(const u8 *p, u32 bytes)
{
    if (bytes != EQ_AUTOMATE_BYTES || p[0] >= 8 || p[1] >= EQ_AUTOMATE_FIELDS) return;
    u32 field = p[1], value = (p[2] & 15u) | (p[3] & 15u) << 4 | (p[4] & 15u) << 8 | (p[5] & 15u) << 12;
    struct eq_value next = eq_channels[p[0]];
    if (field < EQ_AUTOMATE_GAIN)
        next.frequency[field] = eq_frequency_shift(EQ_FREQUENCY_MIN, value*(1.0f/EQ_CENTS_PER_OCTAVE),
                                                   EQ_FREQUENCY_MIN, EQ_FREQUENCY_MAX);
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
