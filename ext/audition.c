#include "ext.h"

/* Audition: a zone is heard once, as when the sample browser loads a file
 * into it.
 * The requester's note plays the zone as the preset would, so a looping
 * channel would go round for as long as the note lasts. While a channel is
 * armed the audio interrupt watches its voice and silences it from the frame
 * its playhead comes round after a whole pass; the foreground then ends the
 * voice and disarms once its release has died away. A channel that does not
 * loop ends by itself.
 *
 * The playhead also jumps back when the note starts over the tail of the
 * voice before it, so a jump counts as the end only after the voice has
 * travelled the sample's length. */

#define START_TICKS      5000u /* armed this long with no voice: the note is not coming */
#define PASS_MARGIN      256u  /* frames short of the length that still count as a whole pass */
#define RELEASE_TICKS    10000u /* the longest wait for a stopped voice to fall silent */

static struct {
    volatile u32 armed, muted;
    u32 channel, pair, armed_tick, stopping;
    u32 synced, panel_zone; /* zone-selection sync was on: the panel's zone, put back on disarm */
    /* Interrupt-owned. */
    u32 last, started;
    u64 travelled, length; /* sample frames << AUDIO_STEP_SHIFT */
} audition;

int audition_armed(void) { return audition.armed != 0; }

void audition_frame(s32 *mix, const s32 *steps)
{
    if (!audition.armed) return;
    u32 channel = audition.channel;
    if (!audition.muted && (s32)REG32(LEVEL_WORD_BASE + channel * LEVEL_WORD_STRIDE) >= LEVEL_SILENT) {
        u32 at = REG32(PLAYHEAD_FRAME_BASE + channel * PLAYHEAD_FRAME_STRIDE);
        s32 step = steps[channel];
        if (audition.started && at < audition.last) {
            if (audition.travelled + ((u64)PASS_MARGIN << AUDIO_STEP_SHIFT) >= audition.length) audition.muted = 1;
            audition.travelled = 0;
        }
        audition.travelled += (u32)(step < 0 ? -step : step);
        audition.started = 1; audition.last = at;
    }
    if (audition.muted) {
        mix[channel] = 0;
        if (audition.pair) mix[channel + 1] = 0;
    }
}

void audition_disarm(u32 channel)
{
    if (!audition.armed || (channel != audition.channel && channel < PRESET_CHANNELS)) return;
    audition.armed = 0;
    if (audition.synced) BYTE(ZONE_EDITOR_CHANNEL_ZONES + audition.channel) = (u8)audition.panel_zone;
    audition.synced = 0;
    audio_hook_update();
}

/* Arm one pass of the zone on its channel: from the zone's start to where
 * its loop comes round, or its end. A Stereo/Right partner is silenced with
 * the channel. */
u32 audition_arm(u32 channel, u32 zone)
{
    const u8 *z = zone_record(channel, zone), *sample = z[0] ? ready_sample(channel, zone) : 0;
    audition_disarm(PRESET_CHANNELS);
    if (!sample) return BAD_STATE;
    u32 frames = REG32((u32)sample + SAMPLE_FRAMES), start = get_u32(z + ZONE_SAMPLE_START), end = get_u32(z + ZONE_SAMPLE_END);
    u32 loop = get_u32(z + ZONE_LOOP_START), length = get_u32(z + ZONE_LOOP_LENGTH) >> ZONE_LOOP_LENGTH_SHIFT;
    if (end > frames) end = frames;
    if (loop >= start && loop < end && length && length < end - loop) end = loop + length;
    /* With zone-selection sync on, the note would play the panel's zone. */
    audition.synced = (s32)REG32(ZONE_EDITOR_SELECTION_SYNC_INDEX) >= 0;
    if (audition.synced) {
        audition.panel_zone = BYTE(ZONE_EDITOR_CHANNEL_ZONES + channel);
        BYTE(ZONE_EDITOR_CHANNEL_ZONES + channel) = (u8)zone;
    }
    audition.channel = channel;
    audition.pair = channel + 1 < PRESET_CHANNELS && channel_record(channel + 1)[CHANNEL_MODE] == CHANNEL_MODE_STEREO_RIGHT;
    audition.length = (u64)(end > start ? end - start : frames) << AUDIO_STEP_SHIFT;
    audition.travelled = 0; audition.started = 0; audition.last = 0; audition.muted = 0; audition.stopping = 0;
    audition.armed_tick = REG32(TICKS);
    MEMORY_BARRIER();
    audition.armed = 1;
    audio_hook_update();
    return OK;
}

void audition_foreground(void)
{
    if (!audition.armed) return;
    if (audition.muted) {
        /* Stay silent through the release: the voice still goes round. */
        u32 channel = audition.channel;
        if (!audition.stopping) { channel_stop(channel); audition.stopping = 1; audition.armed_tick = REG32(TICKS); }
        else if ((s32)REG32(LEVEL_WORD_BASE + channel * LEVEL_WORD_STRIDE) < LEVEL_SILENT ||
                 (u32)(REG32(TICKS) - audition.armed_tick) >= RELEASE_TICKS) audition_disarm(channel);
    } else if (audition.started) {
        /* A voice that ended by itself: the audition is over. */
        if ((s32)REG32(LEVEL_WORD_BASE + audition.channel * LEVEL_WORD_STRIDE) < LEVEL_SILENT) audition_disarm(audition.channel);
    } else if ((u32)(REG32(TICKS) - audition.armed_tick) >= START_TICKS) audition_disarm(audition.channel);
}

void audition_init(void) { audition.armed = 0; }
