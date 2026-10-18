#ifndef PATCH_EQ_H
#define PATCH_EQ_H
#include "patch.h"
#define EQ_EDITOR 0x800dba88u
#define EQ_MODEL 0x80114a64u
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
#define EQ_FREQUENCY_STEPS 240 /* encoder clicks across the range, ~1/24 octave */
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
    u16 hpf; signed short gain[3]; u16 frequency[3]; u8 on, override;
    signed char source[EQ_MOD_COUNT], amount[EQ_MOD_COUNT];
    u8 q[3], reserved; /* reserved stays zero: values compare by memcmp */
};
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
struct extras { struct eq_value channels[8], zones[8][8]; struct grain_settings grain; };
extern struct extras patch_live;
#define eq_channels patch_live.channels
#define eq_zones patch_live.zones
#define patch_grain patch_live.grain
void eq_coefficients(const struct eq_value *value, s32 out[5][5]);
/* A band's gain limit for its shape, and every gain brought inside it. */
s32 eq_gain_limit(const struct eq_value *value, u32 band);
void eq_limit(struct eq_value *value);
/* A bell step's Q in hundredths. */
u32 eq_q_hundredths(u32 q);
u16 eq_frequency(float position);
float eq_frequency_position(u16 frequency);
u16 eq_frequency_step(u16 frequency, s32 delta);
u16 eq_frequency_shift(u16 frequency, float octaves, u16 low, u16 high);
void eq_curve(const struct eq_value *value, u32 points, u8 *out, s32 top, s32 bottom);
void eq_modulate(const struct eq_value *value, u32 channel, struct eq_value *out);
/* A route's CV as the foreground last held it: EQ_CV_COUNTS_FULL is 5 V. A
 * relative source (CV A..CV C) reads `channel`'s inputs; Off reads zero. */
#define EQ_CV_COUNTS_FULL 32768
s32 eq_cv_counts(s32 source, u32 channel);
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
