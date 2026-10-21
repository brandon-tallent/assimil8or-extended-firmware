#ifndef PATCH_EQ_H
#define PATCH_EQ_H
#include "patch.h"
#define EQ_EDITOR 0x800dba88u
#define EQ_MODEL 0x80114a64u
#define EQ_ZONE_SELECTOR_BASE 0x9ffff580u  /* ENGINE_CHANNEL_ZONE_SELECTOR_BASE */
#define EQ_CHANNEL_RECORDS (EQ_MODEL + 0x20u) /* CURRENT_WORKING_PRESET_FIRST_CHANNEL */
#define EQ_CHANNEL_MODE 0x264u              /* PRESET_CHANNEL_MODE */
#define EQ_CHANNEL_MODE_STEREO_RIGHT 2u
#define EQ_ZONE_MASK 7u
#define EQ_CHANNEL_BYTES 0x2c8u
#define EQ_SECTION_COUNT 5u
/* Gain in 0.1 dB. A bell reaches +/-18 dB, the inter-section headroom; a
 * shelf +/-10 dB, where its L1 sum nears the accumulator's sixteen. Every
 * band sweeps the whole audio range; tools/eq_coefficients.py holds the
 * Q4.28, L1 and pole checks for both shapes across it. */
#define EQ_GAIN_MAX 180
#define EQ_SHELF_GAIN_MAX 100
/* A band's q: 0 is its own shape (a shelf for Low and High, the Mid bell
 * whose Q follows its gain); 1..EQ_Q_STEPS is a bell of
 * Q = 0.5 * 2^((q-1)/EQ_Q_STEPS_PER_OCTAVE), 0.5 to 16. */
#define EQ_Q_OWN 0u
#define EQ_Q_STEPS 41u
#define EQ_Q_STEPS_PER_OCTAVE 8
#define EQ_BAND_MID 1u
/* The response graph's range either side of 0 dB. */
#define EQ_GRAPH_DB 21
#define EQ_FREQUENCY_MIN 20u
#define EQ_FREQUENCY_MAX 20000u
/* A band frequency or the filter cutoff is a step of 1/EQ_STEPS_PER_OCTAVE
 * octave, the stored value and the encoder's unit, so a step up and back
 * always returns. 1 kHz is step EQ_STEP_1K, which puts every octave from it
 * (125 Hz, 250 Hz ... 16 kHz) on a step. Step 0 is 20.03 Hz, and the last
 * step is held at EQ_FREQUENCY_MAX. */
#define EQ_STEPS_PER_OCTAVE 120
#define EQ_STEP_1K 677
#define EQ_STEP_MAX 1196
#define EQ_STEP_HZ_1K 1000
#define EQ_STEP_150HZ 349     /* 150.35 Hz, the Low band's default */
#define EQ_STEP_10K 1076      /* 10.02 kHz, the High band's default */
#define EQ_HPF_MIN 20u
#define EQ_HPF_MAX 350u
#define EQ_UNITY 268435456
/* Modulation routes, in CV page order. */
enum { EQ_MOD_LOW_FREQUENCY, EQ_MOD_LOW_GAIN, EQ_MOD_MID_FREQUENCY, EQ_MOD_MID_GAIN,
       EQ_MOD_HIGH_FREQUENCY, EQ_MOD_HIGH_GAIN, EQ_MOD_HPF, EQ_MOD_COUNT };
#define EQ_CV_SOURCE_OFF -4          /* CV_INPUT_SOURCE_OFF */
#define EQ_CV_SOURCE_RELATIVE_FIRST -3 /* CV A..CV C: the channel's own inputs */
#define EQ_CV_SOURCE_MAX 23          /* 1A..8C */
#define EQ_CV_AMOUNT_MAX 100         /* percent; +1.00 */
/* At +1.00 a volt moves a frequency one octave or a gain 2 dB. */
#define EQ_CV_GAIN_TENTHS_PER_VOLT 20
struct eq_value {
    u16 hpf; signed short gain[3]; u16 frequency[3] /* steps, EQ_STEP_* */; u8 on, override;
    signed char source[EQ_MOD_COUNT], amount[EQ_MOD_COUNT];
    u8 q[3], reserved; /* reserved stays zero: values compare by memcmp */
};
/* The channel filter (filter.S; eq_model.c publishes it, eq_math.c designs it): a type, the cutoff as
 * a step on the EQ's log scale (EQ_STEP_*), a resonance and an extra control
 * whose meaning depends on the type, and a source and amount for the cutoff,
 * resonance, extra and type routes as an EQ route's, and on a stereo pair
 * the Stereo/Right lane's cutoff as steps from this one's, so it follows
 * it. A zone that follows its channel is all zero, as an EQ zone is, and
 * values compare by memcmp (the record has no padding). */
enum { FILTER_OFF, FILTER_LP24, FILTER_LP12, FILTER_BP24, FILTER_BP12, FILTER_HP24, FILTER_HP12,
       FILTER_NOTCH24, FILTER_NOTCH12, FILTER_BAND_STOP, FILTER_DUAL, FILTER_TYPES };
enum { FILTER_MOD_CUTOFF, FILTER_MOD_RESONANCE, FILTER_MOD_EXTRA, FILTER_MOD_TYPE, FILTER_MOD_COUNT };
#define FILTER_RESONANCE_MAX 100u
#define FILTER_EXTRA_MAX 100u
/* What a route moves at +1.00 per volt: 5 V takes the cutoff across the
 * whole 20 Hz..20 kHz range, Resonance and Extra across theirs, and Type
 * through every type. */
#define FILTER_CUTOFF_OCTAVES_PER_VOLT 2
#define FILTER_STEPS_PER_VOLT 20
#define FILTER_TYPE_STEPS_PER_VOLT 2
struct filter_value {
    u8 type, resonance, extra, override;
    u16 cutoff;
    signed char source[FILTER_MOD_COUNT], amount[FILTER_MOD_COUNT];
    s16 right;  /* -EQ_STEP_MAX..EQ_STEP_MAX */
};
_Static_assert(sizeof(struct filter_value) == 16, "filter values compare by memcmp: no padding");
/* Right-side cutoff: cutoff + right, clamped to 0..EQ_STEP_MAX. */
static inline u32 filter_right_cutoff(const struct filter_value *value)
{
    s32 step = (s32)value->cutoff + value->right;
    return step < 0 ? 0u : step > (s32)EQ_STEP_MAX ? EQ_STEP_MAX : (u32)step;
}
/* What Extra sets for a type, or null when the type has none. */
const char *filter_extra_name(u32 type);
struct eq_group { s32 mask[4], coefficient[5][5][4]; };
struct eq_bank { struct eq_group group[2]; };
struct eq_history_group { s32 mask[4]; struct { s32 coefficient[5][4], history[5][4]; } section[5]; };
extern struct eq_history_group eq_fixed_data[2];
struct eq_mailbox_state { struct eq_bank *active, *pending; u32 remaining; };
extern struct eq_mailbox_state eq_mailbox;
extern s32 eq_steps[2][5][5][4];
/* A preset's settings beyond the stock model (preset_extras.c): the live
 * ones, and what each library slot holds. A zone that follows its channel
 * is all zero, so two records compare by memcmp. */
struct extras { struct eq_value channels[8], zones[8][8]; struct grain_settings grain[8];
                struct filter_value filters[8], zone_filters[8][8]; };
extern struct extras patch_live;
#define eq_channels patch_live.channels
#define eq_zones patch_live.zones
#define patch_grains patch_live.grain
#define filter_channels patch_live.filters
#define filter_zones patch_live.zone_filters
void eq_coefficients(const struct eq_value *value, s32 out[5][5]);
/* A band's gain limit for its shape, and every gain brought inside it. */
s32 eq_gain_limit(const struct eq_value *value, u32 band);
void eq_limit(struct eq_value *value);
/* A bell step's Q in hundredths. */
u32 eq_q_hundredths(u32 q);
/* A step's frequency in hundredths of a hertz; the step nearest a frequency
 * in hundredths of a hertz, or a number of cents above EQ_FREQUENCY_MIN; and
 * a step's place, 0..1, across the graphs' 20 Hz..20 kHz log scale. */
u32 eq_step_centihertz(u32 step);
u32 eq_step_nearest(u32 centihertz);
u32 eq_step_above_min(u32 cents);
float eq_frequency_position(float hz);
float eq_step_position(u32 step);
/* A step as the pages show it, without a unit: tenths of a hertz below
 * 200 Hz, whole hertz, then kHz to thousandths and above 10 kHz to
 * hundredths, so each step reads differently. Nonzero when it is in kHz. */
u32 eq_step_text(char *out, u32 step);
u16 eq_frequency_shift(u16 frequency, float octaves, u16 low, u16 high);
void eq_curve(const struct eq_value *value, u32 points, u8 *out, s32 top, s32 bottom);
void eq_modulate(const struct eq_value *value, u32 channel, struct eq_value *out);
/* A route's CV as the foreground last held it: EQ_CV_COUNTS_FULL is 5 V. A
 * relative source (CV A..CV C) reads `channel`'s inputs; Off reads zero. */
#define EQ_CV_COUNTS_FULL 32768
s32 eq_cv_counts(s32 source, u32 channel);
/* The input a source reads for channel, 0..23, or -1 for Off. */
s32 eq_cv_input(s32 source, u32 channel);
u32 eq_modulated(const struct eq_value *value);
void eq_foreground_poll(void);
/* CMD_EQ_AUTOMATE: one channel EQ field from a sequencer, without a reply.
 * u8 channel, u8 field, then the value as four nibbles, low first. Values
 * count from zero: a frequency in cents above EQ_FREQUENCY_MIN, a gain in
 * dB above -EQ_GAIN_MAX, a band's q, the HPF in Hz (0 Off), EQ On. */
enum { EQ_AUTOMATE_FREQUENCY, EQ_AUTOMATE_GAIN = EQ_AUTOMATE_FREQUENCY + 3, EQ_AUTOMATE_Q = EQ_AUTOMATE_GAIN + 3,
       EQ_AUTOMATE_HPF = EQ_AUTOMATE_Q + 3, EQ_AUTOMATE_ON, EQ_AUTOMATE_FIELDS };
#define EQ_AUTOMATE_BYTES 6u
#define EQ_CENTS_PER_OCTAVE 1200
#define EQ_TENTHS_PER_DB 10
void eq_changed(void);
void filter_changed(void);
void filter_default(struct filter_value *value, u32 channel);
void filter_foreground_poll(void);
/* One lane of the filter's published vectors (filter.S) for a setting: the
 * lane mask, section 1's a1 a2 a3 and k/4, section 2's, the two tap mixes
 * (input, lowpass, bandpass, highpass each, as Q30 so a weight reaches 2),
 * the lanes whose cutoff its CV moves, with their sections' base g and k
 * (floats), the cutoff CV's input (a byte offset into the calibrated CV
 * block) and octaves per count (float), and the weight of section 1's
 * lowpass in the output, which runs section 2 beside it rather than after
 * it. The last vector holds a group's flags. */
enum { FILTER_VECTOR_MASK, FILTER_VECTOR_SECTION_1, FILTER_VECTOR_SECTION_2 = 5, FILTER_VECTOR_MIX_1 = 9,
       FILTER_VECTOR_MIX_2 = 13, FILTER_VECTOR_DYNAMIC_MASK = 17,
       FILTER_VECTOR_G1, FILTER_VECTOR_K1, FILTER_VECTOR_CV_INPUT, FILTER_VECTOR_CV_SCALE,
       FILTER_VECTOR_G2, FILTER_VECTOR_K2, FILTER_VECTOR_MIX_2_LP1, FILTER_VECTOR_FLAGS, FILTER_VECTORS };
/* A group's flags, one word each: a lane's cutoff moves with its CV. */
enum { FILTER_FLAG_DYNAMIC };
void filter_lane(const struct filter_value *value, s32 out[FILTER_VECTORS]);
void filter_curve(const struct filter_value *value, u32 points, u8 *out, s32 top, s32 bottom, s32 db_top, s32 db_bottom);
/* The setting as channel's routes move it now, cutoff included, for the
 * page's graph; the renderer moves the cutoff itself each block. */
void filter_live(const struct filter_value *value, u32 channel, struct filter_value *out);
u32 filter_modulated(const struct filter_value *value);
/* A DATA 2 position (0..1 as Q16) on the log scale of frequencies. */
void eq_default(struct eq_value *value);
void preset_extras_reset(void);
void preset_extras_foreground(void);
u32 eq_panel_active(void);
void eq_panel_forget(void);
void eq_panel_enter(u32 *context, u32 argument);
void eq_panel_after_bits(u32 *context);
void eq_panel_knob(u32 *context, u32 bits);
void eq_panel_render(u32 *task);
void eq_panel_select(u32 *context, s32 delta);
#endif
