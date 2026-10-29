/* Time Lock's shared layouts: the interrupt engine (timelock.c) and the
 * foreground's join search (joins.c). */
#ifndef TIMELOCK_H
#define TIMELOCK_H

#define PREROLL_FRAMES        96u     /* 2 ms */
#define ATTACK_FRAMES         1200u   /* 25 ms of each slice played as the resampled hit */
#define SETTLE_FRAMES         288u    /* 6 ms: a fill reuses no earlier part of the slice */
#define FILL_FADE_FRAMES      384u    /* 8 ms */
#define FILL_FADE_MIN         48u     /* 1 ms */
#define FILL_FADE_FLOOR       36u     /* 0.75 ms: below it the slice plays on */
#define PIECE_FRAMES          864u    /* 18 ms, varied by a quarter either way */
#define PIECE_JITTER_SHIFT    2u
#define PROTECT_FRAMES        960u    /* 20 ms: a jump back lands this far after the last rise played */
#define RUNOUT_FRAMES         240u    /* 5 ms fade where a slice's own audio runs out */
#define FADES_MAX             FILL_FADE_FRAMES
#define STREAMS               2u      /* reads fading out at once on a channel */
#define EVENTS_PER_FRAME      1u      /* joins and slice starts in one frame, across the channels */
#define FILL_STREAMS_MAX      3u      /* fill fades at once across the channels; a fill join waits for room */
#define FILL_WAIT_MAX         96      /* but at most 2 ms, then starts anyway */
#define EVENT_BUDGET_CYCLES   1200u   /* an event waits a frame once the frame's work has taken this much */
#define SKIP_Q32              (1u << 20) /* a write this close to the voice is skipped */
#define FOLLOW_Q32            (2ull << 32) /* the voice is where it was expected within 2 frames */
#define OUTPUT_HZ             48000u
#define ONE_Q30               (1u << 30)
#define Q31_MAX               0x7fffffff
#define Q31_MIN               (-Q31_MAX - 1)
#define ONE_Q24               (1u << 24)
#define SINE_STEPS            256u    /* the quarter sine table */
#define SAMPLE_SHIFT          4u      /* Q31 PCM to Q27 for the cubic */
#define READ_MARGIN           4u      /* frames the cubic reads around a position */
#define POSITION_NONE         0xffffffffu
#define DUE_NONE              0x7fffffff
#define ONSET_NONE            0x7fffffffffffffffll
#define COLD                  __attribute__((noinline, section(".text.timelock_events")))
#define WARM_FRAMES           8       /* frames before an event that its code and lines start to be preloaded */
#define WARM_LINES            8u      /* lines preloaded a frame */
#define RANDOM_SEED           0x2545f491u
/* timelock_set's fields, from Set parameter SET_TIME_LOCK (editor.c). */
enum { TIMELOCK_SET_ON, TIMELOCK_SET_PLAYING, TIMELOCK_SET_TEMPO, TIMELOCK_SET_SPEED, TIMELOCK_SET_INPUT,
       TIMELOCK_SET_PPQN, TIMELOCK_SET_BEATS, TIMELOCK_SET_FIELDS };
#define PLAYHEAD_VECTOR       0x9ffff9c0u /* AUDIO_RENDER_PLAYHEAD_VECTOR_BASE: 64-bit, frame in the high word */
#define PLAYHEAD_BYTES        8u
#define STEP_TO_Q32           (32u - AUDIO_STEP_SHIFT)
#define ZONE_SELECTOR_BASE    0x9ffff580u /* ENGINE_CHANNEL_ZONE_SELECTOR_BASE */
#define ZONE_SELECTOR_STRIDE  4u
#define ZONE_SELECTOR_MASK    7u
#define CHANNEL_REVERSE       0x265u
#define CHANNEL_LOOP_OFF      0u
/* Tempo: the clocks Time Lock follows. */
#define CLOCK_INPUT_NONE      0xffu   /* a CV clock no channel follows */
#define CLOCKS                (1u + PRESET_CHANNELS) /* MIDI, then each channel's CV input */
#define MIDI_PPQN             24u
#define CLOCK_HIGH            9830    /* 1.5 V: a CV clock's pulse starts */
#define CLOCK_LOW             4915    /* 0.75 V: and ends */
#define CLOCK_RING            128u    /* tick times kept; the period averages up to CLOCK_BEATS of them */
#define CLOCK_BEATS           4u      /* MIDI ticks arrive up to 1 ms late: four beats average that out */
#define CLOCK_SNAP_FRACTION   5u      /* an interval a fifth off the average starts a new one */
#define CLOCK_LATE_PERIODS    3u      /* no tick for this many periods: the clock stopped */
#define CLOCK_PERIOD_MIN_FRAMES 16u   /* a shorter measured period is ticks read together */
#define CLOCK_FRAME_TICKS_MAX 4u      /* more MIDI ticks in one frame: a backlog, counted as one */
#define CLOCK_GAP_FRAMES      96000u  /* 2 s without a tick: the next tick measures nothing */
#define HUNDREDTHS            100u
#define BPM_HOLD_HUNDREDTHS   8       /* the BPM shown holds while the clock stays this close to it */
#define CLOCK_PHASE_SHIFT     3       /* each tick corrects an eighth of the position's error */
#define CLOCK_LEAD_TICKS      2u      /* the position runs at most this far past the last tick */
#define SPEED_RANGE_SHIFT     4u      /* a clocked speed stays within 16 times the sample's own */
#define SECONDS_PER_MINUTE    60u
#define TENTHS                10u
#define NAME_BPM_LOW          60u     /* a number in a sample's name read as its tempo */
#define NAME_BPM_HIGH         200u
#define NAME_TEMPO_DIGITS     3u
#define GUESS_BPM_LOW         90u     /* with no tempo in the name, Beats puts the loop at 90 to 180 BPM */
#define ASCII_LOWER           0x20
#define FNV_BASIS             0x811c9dc5u
#define FNV_PRIME             0x01000193u

/* A zone as the interrupt plays it, published by the foreground. */
struct lock_zone {
    const s32 *pcm[2];      /* frame 0 of the buffers the channel and its Stereo/Right partner read */
    const u32 *positions;   /* the slice map: sorted frames, HIT_FLAG on slice starts */
    u32 count;              /* 0: no map yet, and the zone plays as with Time Lock off */
    u32 first, end;         /* the zone's frames in the sample */
    u32 loop, turn;         /* where the loop starts and the frame it comes round at; turn == end: none */
    s32 base;               /* Q25 frames per output frame at the sample's own speed */
    u32 inverse;            /* 2^56 / base: output frames from sample frames, and step / base */
    u16 loop_index;         /* the first position at or past the loop's start */
    s64 tick;               /* Q32 sample frames a clock tick moves the timeline; 0 without a clock */
};
struct lock_table {
    struct lock_zone zone[PRESET_CHANNELS][ZONES];
    u8 on[PRESET_CHANNELS], pair[PRESET_CHANNELS];
    u8 clock[PRESET_CHANNELS];      /* the timelock_clocks index a channel follows, plus one; 0: none */
    u8 clock_input[CLOCKS];         /* the CV input each CV clock reads, or CLOCK_INPUT_NONE */
    u32 own[PRESET_CHANNELS]; /* Q16 base / step, the slice's own audio against its length; 0 not moving */
};

struct fade { u32 phase, step; u8 law; };
/* A join the foreground worked out ahead: for event `key`, of `kind`, the
 * start as an offset from the timeline, and the crossfade law. */
enum { PLAN_NONE, PLAN_ONSET, PLAN_FILL };
struct plan { volatile u32 key; volatile u8 kind, law; s64 offset; }; /* Q16 through the fade; law Q8, 0 equal power to 256 equal gain */
struct stream { s64 position; struct fade out, in; u8 active; };
enum { MODE_PLAY_ON, MODE_FILL, MODE_RUNOUT, MODE_WAIT, MODE_END };
/* A lock works on an unrolled timeline: positions keep growing as the
 * voice goes round its loop. A real frame plus `lap` is the unrolled one. */
struct lock {
    /* Every frame: one cache line, and the next with a clock. */
    u8 engaged, zone, mode, mixing; /* mixing: a fade, a read fading out, or the run-out */
    u8 warmed, waited, anchored, pad; /* the next event's lines preloaded so far; frames the fill join has waited; tick_at holds */
    s32 due;                /* the output frame of the slice's next event (a fill piece, the run-out) */
    s32 o;                  /* output frames since a */
    u32 cursor;             /* the first position past a, in a's lap */
    s64 timeline, expect;   /* Q32: the unrolled timeline; the voice's real playhead next frame */
    s64 onset_at;           /* the timeline where the next slice's pre-roll starts */
    s64 lap;                /* Q32: unrolled minus real, for the voice */
    /* With a clock: the clock's position (Q32 ticks) at the anchor, the
     * timeline there, and the clock's epoch. */
    u64 position_at;
    s64 tick_at;
    u8 epoch;
    /* At events and while mixing. */
    s64 a, b;               /* Q32 unrolled: the slice's start and the next slice's */
    s64 a_lap, b_lap;       /* the laps of a's and b's positions */
    u32 b_hit, b_index;     /* b starts a slice, and its position's index */
    u32 length, own;        /* the slice in output frames, and its own audio at the pitch */
    u32 attack, xfade;      /* where the fill starts, and its fade */
    u32 piece;              /* the fill's piece length */
    u32 runout_phase;       /* Q16 of the run-out fade, then held at zero */
    u32 next_piece;         /* the next fill piece's length, drawn a join ahead for the planner */
    volatile u32 serial;    /* events so far: the planner's key */
    struct fade in;         /* the voice's own fade-in after a join */
    struct stream streams[STREAMS];
    struct plan plan;       /* the foreground's search for event `serial` */
} __attribute__((aligned(CACHE_LINE_BYTES)));

extern struct lock_table timelock_tables[2];
extern volatile u32 timelock_live;
extern struct lock timelock_locks[PRESET_CHANNELS];
u32 timelock_after(const struct lock_zone *z, u32 frame);
/* A clock measured in the audio interrupt; `position` is phase-locked to
 * its ticks, each tick correcting an eighth of the error. */
struct clock {
    u64 position;           /* Q32 ticks: the phase-locked position playback follows */
    u32 seen, age, period_q8, inverse, source, shown10;
    u32 times[CLOCK_RING];
    u8 primed, high, count, next, window, settle, measuring, epoch;
    signed char off;               /* the last interval was long (1), short (-1) or near the period */
};
extern struct clock timelock_clocks[CLOCKS];
void joins_plan(u32 channel, const struct lock_table *t);

#endif
