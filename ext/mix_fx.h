/* Mix effects: a compressor on each channel and on the mix, and two sends
 * on the mix, a reverb and a tempo-synced delay. mix_fx.c runs them in the
 * audio interrupt; mix_page.c edits the settings. */
#ifndef MIX_FX_H
#define MIX_FX_H

#define MIX_CHANNELS        8u
#define MIX_RATIOS          9u   /* Off, 1.5:1, 2:1, 3:1, 4:1, 6:1, 10:1, 20:1, Limit */
#define MIX_ATTACKS         11u
#define MIX_RELEASES        7u
#define MIX_THRESHOLD_MAX   40u  /* dB below full scale */
#define MIX_KEY_SELF        0u   /* 1-8: that channel */
#define MIX_KEY_INPUT       9u   /* the Sample input, left and right */
#define MIX_DIVISIONS       11u
#define MIX_FILTER_STEPS    7u
#define MIX_PERCENT_MAX     100u
#define MIX_FEEDBACK_MAX    120u
#define MIX_DECAY_MIN       2u   /* tenths of a second */
#define MIX_DECAY_MAX       100u
#define MIX_BPM_MIN         40u
#define MIX_BPM_MAX         240u
/* The delay's Tempo: MIDI clock, a channel's Time Lock tempo, or a BPM. */
#define MIX_TEMPO_MIDI      0u
#define MIX_TEMPO_CHANNEL   1u   /* 1-8: channels 1-8 */
#define MIX_TEMPO_BPM       9u   /* and above: MIX_BPM_MIN + (tempo - MIX_TEMPO_BPM) */
#define MIX_TEMPO_MAX       (MIX_TEMPO_BPM + MIX_BPM_MAX - MIX_BPM_MIN)

/* A CV route: an input selector (EQ_CV_SOURCE_*) and an amount, -100 to
 * 100. At +100, 5 V moves the setting across its whole range. */
struct mix_route { signed char source, amount; };
enum { MIX_ROUTE_REVERB_SEND, MIX_ROUTE_DELAY_SEND, MIX_SEND_ROUTES };
enum { MIX_ROUTE_REVERB_DECAY, MIX_ROUTE_REVERB_DAMP, MIX_ROUTE_REVERB_LEVEL, MIX_ROUTE_DELAY_TIME,
       MIX_ROUTE_DELAY_FEEDBACK, MIX_ROUTE_DELAY_LOW, MIX_ROUTE_DELAY_HIGH, MIX_ROUTE_DELAY_LEVEL,
       MIX_ROUTE_CHORUS_RATE, MIX_ROUTE_CHORUS_DEPTH, MIX_BUS_ROUTES };
/* The reverb's algorithms, in the DEP-5's order and numbering (10, chorus
 * and delay, is left out: the delay send covers it). */
#define MIX_ALGOS           10u
#define MIX_ALGO_GATED      6u   /* this index and above: Decay is the gate length */

/* One compressor's settings. */
/* One compressor's settings: Drive (a soft-clipping gain ahead of it),
 * Gain (makeup, dB) and Mix (wet percent, for parallel compression). */
struct mix_comp_set { u8 ratio, threshold, attack, release, drive, gain, mix, key; };
#define MIX_GAIN_MAX        24u

/* The settings live in the preset's extras (patch_live.extension), saved
 * as MixFX lines. A preset without them has zeros there: the defaults
 * apply until the first edit writes them. MIX_FORMAT changes when a field
 * moves; fields appended later read zero from older presets. */
#define MIX_FORMAT          1u
struct mix_settings {
    u8 format;
    struct mix_comp_set comp[MIX_CHANNELS], master;
    u8 reverb_send[MIX_CHANNELS], delay_send[MIX_CHANNELS];
    u8 reverb_algo, reverb_decay, reverb_damp, reverb_level, chorus_rate, chorus_depth;
    u8 delay_division, delay_feedback, delay_low, delay_high, delay_level, delay_tempo;
    struct mix_route send_route[MIX_CHANNELS][MIX_SEND_ROUTES], bus_route[MIX_BUS_ROUTES];
    u8 send_pre[MIX_CHANNELS];   /* 1: sends before the channel's level and mute; 0: after */
};
_Static_assert(sizeof(struct mix_settings) <= EXTRAS_EXTENSION_BYTES, "mix settings outgrow the preset's extension bytes");
extern struct mix_settings mix_defaults;
/* The settings in effect: the preset's, or the defaults. */
static inline const struct mix_settings *mix_now(void)
{
    const struct mix_settings *live = (const struct mix_settings *)patch_live.extension;
    return live->format == MIX_FORMAT ? live : &mix_defaults;
}
/* A channel's part, which moves with stock channel copies (records.c). */
struct mix_channel { struct mix_comp_set comp; u8 reverb_send, delay_send, send_pre; struct mix_route send_route[MIX_SEND_ROUTES]; };
void mix_channel_get(const struct mix_settings *from, u32 channel, struct mix_channel *out);
void mix_channel_put(u32 channel, const struct mix_channel *in);
/* The preset's settings to change, starting from the defaults. */
struct mix_settings *mix_edit(void);


extern const char *const mix_ratio_names[MIX_RATIOS];
extern const u16 mix_attack_hundredths_ms[MIX_ATTACKS];
extern const u16 mix_release_ms[MIX_RELEASES];
extern const char *const mix_division_names[MIX_DIVISIONS];
extern const char *const mix_algo_names[MIX_ALGOS];
/* Whether an algorithm has a chorus. */
u32 mix_algo_chorus(u32 algo);
/* A compressor's gain reduction now, whole dB: a channel, or MIX_CHANNELS for the mix. */
u32 mix_fx_reduction_db(u32 compressor);
/* The delay's cut frequencies in Hz; 0 is Off. */
extern const u16 mix_low_cut_hz[MIX_FILTER_STEPS], mix_high_cut_hz[MIX_FILTER_STEPS];

/* Remote edits (editor.c's Set, IDs from SET_MIX_FIRST): a compressor's
 * fields in struct order (channel byte 0-7, or MIX_CHANNELS for the mix),
 * a channel's sends and send routes, the twelve reverb and delay bytes from
 * reverb_algo in struct order, the bus routes, then Status, which only
 * reads. A route is its source, then its amount. */
#define MIX_BUS_BYTES 12u /* reverb_algo to delay_tempo */
enum {
    MIX_SET_COMP, MIX_SET_REVERB_SEND = MIX_SET_COMP + sizeof(struct mix_comp_set), MIX_SET_DELAY_SEND,
    MIX_SET_SEND_PRE, MIX_SET_SEND_ROUTE, MIX_SET_BUS = MIX_SET_SEND_ROUTE + 2 * MIX_SEND_ROUTES,
    MIX_SET_BUS_ROUTE = MIX_SET_BUS + MIX_BUS_BYTES, MIX_SET_STATUS = MIX_SET_BUS_ROUTE + 2 * MIX_BUS_ROUTES, MIX_SET_IDS
};
/* Status: each compressor's gain reduction in whole dB (eight channels,
 * then the mix), then u16 the delay's tempo in tenths of BPM. */
#define MIX_STATUS_BYTES (MIX_CHANNELS + 1u + 2u)
u32 mix_set(u32 channel, u32 id, s32 value, u8 *out, u32 *count);

/* Marks the settings changed; the audio interrupt recalculates them. */
void mix_fx_update(void);
/* The tempo the delay follows, in tenths of BPM; 0 when its clock has stopped. */
u32 mix_fx_bpm10(void);
/* Follows the delay's tempo source, from the foreground. */
void mix_fx_foreground(void);
int mix_fx_wants_midi_clock(void);
void mix_fx_init(void);
void mix_page_init(void);

#endif
