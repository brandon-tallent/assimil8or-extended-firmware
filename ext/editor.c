#include "ext.h"

/* Remote preset editing. The panel context only queues one
 * request; the foreground runs it with the stock shared UI/sample-operation
 * slot claimed, so model edits never interleave with a preset load, a sample
 * service job or another remote operation. Edits store the value the stock
 * setter would store and publish through the same stock routine. The preset
 * reads as modified from then on: the stock dirty test compares the working
 * model with its library record. */

enum {
    EDIT_SUMMARY = CMD_EXTENSION_FIRST, EDIT_READ, EDIT_SAMPLE, EDIT_SET, EDIT_ZONE,
    EDIT_ASSIGN, EDIT_FOLDER, EDIT_PEAKS, EDIT_TRANSIENTS, EDIT_EXPLODE, EDIT_SAVE, EDIT_FILE_PEAKS, EDIT_STOP,
};
#define REQUEST_MAX           48u
#define READ_MAX              120u
#define SAVE_TIMEOUT_TICKS    20000u
#define ASSIGN_BROWSED        0x80u /* in Assign's zone byte: the file is in the browsed folder */
#define ASSIGN_AUDITION       0x40u /* and: the channel's voice ends, and its next note plays the sample once */
enum { SAVE_IDLE, SAVE_COPYING, SAVE_WRITING };
#define SUMMARY_MODIFIED      0x01u
#define SUMMARY_STORAGE_BUSY  0x02u

static struct {
    volatile u32 command;
    u32 sequence, bytes, started;
    u8 payload[REQUEST_MAX];
    u32 saving;
} job;

static void init(void) { memset(&job, 0, sizeof(job)); folder_init(); stream_init(); audition_init(); grains_init(); resample_init(); }
static int busy(void) { return job.command != 0; }

/* EDIT_SET with this sequence, which no request uses, is automation. */
#define AUTOMATE_SEQUENCE 0u
static void automate_request(const u8 *p, u32 bytes);
static void command(u32 command, u32 sequence, const u8 *p, u32 bytes)
{
    if (command == EDIT_STREAM) { stream_request(sequence, p, bytes); return; }
    if (command == EDIT_SET && sequence == AUTOMATE_SEQUENCE) { automate_request(p, bytes); return; }
    if (job.command) { reply(command, sequence, BUSY, 0, 0); return; }
    job.sequence = sequence; job.bytes = bytes;
    memcpy(job.payload, p, bytes);
    MEMORY_BARRIER();
    job.command = command;
}

u32 active_zones(u32 channel)
{
    u32 count = 0;
    while (count < ZONES && zone_record(channel, count)[0]) ++count;
    return count;
}
static void publish(u32 channel, u32 zone)
{
    u32 context[ZONE_EDITOR_CONTEXT_WORDS] = {0};
    context[ZONE_EDITOR_SELECTED_CHANNEL / 4] = channel;
    context[ZONE_EDITOR_SELECTED_ZONE / 4] = zone;
    preset_publish_selected_channel_and_linked_stereo(context);
}
/* Structural edits (samples, zone lists) rebuild the whole runtime, as a
 * preset load does. */
static void rebuild(u32 channel, u32 first, u32 zones)
{
    for (u32 zone = first; zone < first + zones; ++zone)
        preset_sync_stereo_right_channel((void *)PRESET_MODEL, channel, zone);
    sample_runtime_rebuild_from_model_and_publish_channels((void *)SAMPLE_RUNTIME_OBJECT, (void *)PRESET_MODEL);
}

/* ---- Parameters: the stock stored forms (PRESET_FORMAT.md). ---- */
struct parameter { u16 offset; u8 bytes, zone; s32 low, high; };
#define CHANNEL_PARAMETERS 16u
#define MOD_FIRST 16u
#define MOD_ASSIGNMENTS 16u
#define MOD_BASE 0x288u
#define SET_CHANNEL_MODE 48u
#define SET_MIX_MOD_SOURCE 49u
#define SET_MIX_MOD_AMOUNT 50u
#define SET_MIX_STATE 51u
#define ZONE_FIRST 64u
/* Channel/zone EQ: the zone byte names a zone, or EQ_SLOT_CHANNEL. A zone
 * that follows its channel copies the channel EQ on its first edit, as the
 * panel does; clearing Override makes it follow again. */
#define SET_EQ_FIRST 96u
enum {
    SET_EQ_HPF, SET_EQ_FREQUENCY, SET_EQ_GAIN = SET_EQ_FREQUENCY + 3, SET_EQ_ON = SET_EQ_GAIN + 3,
    SET_EQ_OVERRIDE, SET_EQ_SOURCE, SET_EQ_AMOUNT = SET_EQ_SOURCE + EQ_MOD_COUNT,
    SET_EQ_Q = SET_EQ_AMOUNT + EQ_MOD_COUNT, SET_EQ_IDS = SET_EQ_Q + 3
};
/* Grain mode fields (grains.c); the zone byte is unused. Resampling
 * (resample.c) follows them. */
#define SET_GRAIN_FIRST 160u
#define SET_RESAMPLE_FIRST 172u
static const struct parameter channel_parameters[CHANNEL_PARAMETERS] = {
    {0x270, 2, 0, -9600, 6000},   /* pitch, cents */
    {0x272, 2, 0, -900, 60},      /* level, 0.1 dB */
    {0x274, 2, 0, -100, 100},     /* pan */
    {0x276, 2, 0, 10, 320},       /* bits, 0.1 bit */
    {0x278, 4, 0, 0, 990000},     /* attack, 0.1 ms */
    {0x27c, 4, 0, 0, 990000},     /* release */
    {0x280, 1, 0, 0, 1},          /* play mode: gated, one shot */
    {0x282, 2, 0, -900, 60},      /* mix level */
    {0x285, 1, 0, 0, 100},        /* aliasing */
    {0x286, 2, 0, 0, 100},        /* PM index */
    {0x284, 1, 0, 0, 10},         /* PM source */
    {0x265, 1, 0, 0, 1},          /* reverse */
    {0x266, 1, 0, 0, 2},          /* loop mode */
    {0x269, 1, 0, 0, 1},          /* attack from current */
    {0x267, 1, 0, 0, 1},          /* loop length is end */
    {0x26b, 1, 0, 0, 1},          /* mix mod is fader */
};
static const struct parameter zone_parameters[] = {
    {ZONE_PITCH_OFFSET, 2, 1, -9600, 6000},
    {ZONE_LEVEL_OFFSET, 2, 1, -900, 60},
    {ZONE_MIN_VOLTAGE, 2, 1, -500, 500},
    {ZONE_SAMPLE_START, 4, 1, 0, (s32)ZONE_POSITION_UNBOUNDED},
    {ZONE_SAMPLE_END, 4, 1, 0, (s32)ZONE_POSITION_UNBOUNDED},
    {ZONE_LOOP_START, 4, 1, 0, (s32)ZONE_POSITION_UNBOUNDED},
    {ZONE_LOOP_LENGTH, 4, 1, ZONE_LOOP_LENGTH_MIN, (s32)ZONE_POSITION_UNBOUNDED},
    {ZONE_SIDE, 1, 1, 0, 1},
};
#define ZONE_PARAMETERS (sizeof(zone_parameters) / sizeof(zone_parameters[0]))

static int parameter_for(u32 id, struct parameter *out)
{
    if (id < CHANNEL_PARAMETERS) { *out = channel_parameters[id]; return 1; }
    if (id >= MOD_FIRST && id < MOD_FIRST + 2 * MOD_ASSIGNMENTS) {
        u32 slot = (id - MOD_FIRST) / 2, amount = (id - MOD_FIRST) % 2;
        struct parameter p = {(u16)(MOD_BASE + slot * 4 + amount * 2), 2, 0, amount ? -100 : -4, amount ? 100 : 23};
        *out = p; return 1;
    }
    if (id == SET_MIX_MOD_SOURCE || id == SET_MIX_MOD_AMOUNT) {
        struct parameter p = {(u16)(CHANNEL_MIX_MOD + (id == SET_MIX_MOD_AMOUNT ? MOD_AMOUNT : 0)), 2, 0,
                              id == SET_MIX_MOD_AMOUNT ? -MOD_AMOUNT_LIMIT : MOD_SOURCE_MIN,
                              id == SET_MIX_MOD_AMOUNT ? MOD_AMOUNT_LIMIT : MOD_SOURCE_MAX};
        *out = p; return 1;
    }
    if (id >= ZONE_FIRST && id < ZONE_FIRST + ZONE_PARAMETERS) { *out = zone_parameters[id - ZONE_FIRST]; return 1; }
    return 0;
}
static s32 load_value(const u8 *at, u32 bytes)
{
    if (bytes == 1) return at[0];
    if (bytes == 2) return (s16)get_u16(at);
    return (s32)get_u32(at);
}
static void store_value(u8 *at, u32 bytes, s32 value)
{
    if (bytes == 1) at[0] = (u8)value;
    else if (bytes == 2) put_u16(at, (u32)value);
    else put_u32(at, (u32)value);
}

static int has_stereo_sample(u32 channel)
{
    for (u32 zone = 0; zone < active_zones(channel); ++zone) {
        const u8 *sample = ready_sample(channel, zone);
        if (sample && REG32((u32)sample + SAMPLE_CHANNELS) == 2) return 1;
    }
    return 0;
}

/* Channel Mode: Master, Link, Stereo/Right or Cycle. Channel 1 always
 * leads. Stereo/Right takes the right half of the channel before, as a
 * stereo sample assignment does, so it needs a stereo sample there and
 * replaces this channel's zones. */
static u32 set_channel_mode(u32 channel, s32 mode)
{
    if (mode < (s32)CHANNEL_MODE_MASTER || mode > (s32)CHANNEL_MODE_CYCLE || (!channel && mode)) return BAD_RANGE;
    u8 *record = channel_record(channel);
    if ((u32)mode == CHANNEL_MODE_STEREO_RIGHT && record[CHANNEL_MODE] != CHANNEL_MODE_STEREO_RIGHT) {
        u32 master = channel - 1, zones = active_zones(master);
        if (channel_record(master)[CHANNEL_MODE] == CHANNEL_MODE_STEREO_RIGHT || !has_stereo_sample(master))
            return BAD_STATE;
        for (u32 zone = 0; zone < ZONES; ++zone) preset_zone_initialize_empty(zone_record(channel, zone));
        record[CHANNEL_MODE] = (u8)mode;
        rebuild(master, 0, zones);
        return OK;
    }
    record[CHANNEL_MODE] = (u8)mode;
    rebuild(channel, 0, 0);
    return OK;
}

static s32 clamp(s32 x, s32 low, s32 high) { return x < low ? low : x > high ? high : x; }

void eq_record(u32 index, u8 *out)
{
    u32 channel = index / EQ_SLOTS, slot = index % EQ_SLOTS;
    const struct eq_value *v = slot == EQ_SLOT_CHANNEL ? &eq_channels[channel] : &eq_zones[channel][slot];
    put_u16(out, v->hpf);
    for (u32 band = 0; band < 3; ++band) {
        put_u16(out + 2 + band * 2, (u32)v->frequency[band]);
        put_u16(out + 8 + band * 2, (u32)v->gain[band]);
    }
    out[14] = v->on; out[15] = v->override;
    for (u32 m = 0; m < EQ_MOD_COUNT; ++m) { out[16 + m] = (u8)v->source[m]; out[16 + EQ_MOD_COUNT + m] = (u8)v->amount[m]; }
    memcpy(out + 16 + 2 * EQ_MOD_COUNT, v->q, 3);
}

/* One EQ field, clamped to the panel's ranges; the stored value is returned.
 * A band that becomes a shelf brings its gain inside the shelf's range. */
static u32 set_eq(u32 channel, u32 slot, u32 id, s32 value, s32 *stored)
{
    if (channel >= PRESET_CHANNELS || slot > EQ_SLOT_CHANNEL || id >= SET_EQ_IDS) return BAD_RANGE;
    if (id == SET_EQ_OVERRIDE && slot == EQ_SLOT_CHANNEL) return BAD_RANGE;
    struct eq_value next = slot == EQ_SLOT_CHANNEL ? eq_channels[channel] : eq_zones[channel][slot];
    if (slot != EQ_SLOT_CHANNEL && !next.override) next = eq_channels[channel];
    next.override = slot != EQ_SLOT_CHANNEL;
    if (id == SET_EQ_HPF) {
        next.hpf = (u16)(value <= 0 ? 0 : clamp(value, EQ_HPF_MIN, EQ_HPF_MAX));
        *stored = next.hpf;
    } else if (id < SET_EQ_GAIN) {
        next.frequency[id - SET_EQ_FREQUENCY] = (u16)clamp(value, EQ_FREQUENCY_MIN, EQ_FREQUENCY_MAX);
        *stored = next.frequency[id - SET_EQ_FREQUENCY];
    } else if (id < SET_EQ_ON) {
        s32 limit = eq_gain_limit(&next, id - SET_EQ_GAIN);
        next.gain[id - SET_EQ_GAIN] = (signed short)clamp(value, -limit, limit);
        *stored = next.gain[id - SET_EQ_GAIN];
    } else if (id == SET_EQ_ON) *stored = next.on = value != 0;
    else if (id == SET_EQ_OVERRIDE) {
        /* A zone that follows its channel again holds nothing. */
        if (!value) memset(&next, 0, sizeof(next));
        *stored = next.override;
    }
    else if (id < SET_EQ_AMOUNT) *stored = next.source[id - SET_EQ_SOURCE] = (signed char)clamp(value, EQ_CV_SOURCE_OFF, EQ_CV_SOURCE_MAX);
    else if (id < SET_EQ_Q) *stored = next.amount[id - SET_EQ_AMOUNT] = (signed char)clamp(value, -EQ_CV_AMOUNT_MAX, EQ_CV_AMOUNT_MAX);
    else {
        *stored = next.q[id - SET_EQ_Q] = (u8)clamp(value, EQ_Q_OWN, EQ_Q_STEPS);
        eq_limit(&next);
    }
    if (slot == EQ_SLOT_CHANNEL) eq_channels[channel] = next;
    else eq_zones[channel][slot] = next;
    return OK;
}

static u32 set_parameter(const u8 *p, u8 *out, u32 *count)
{
    u32 channel = p[0], zone = p[1];
    struct parameter parameter;
    if (p[2] >= SET_GRAIN_FIRST + GRAIN_SET_SLIDE) return grain_set(channel, p[2] - SET_GRAIN_FIRST, (s32)get_u32(p + 3), out, count);
    if (p[2] >= SET_RESAMPLE_FIRST) return resample_set(channel, zone, p[2] - SET_RESAMPLE_FIRST, (s32)get_u32(p + 3), out, count);
    if (p[2] >= SET_GRAIN_FIRST) return grain_set(channel, p[2] - SET_GRAIN_FIRST, (s32)get_u32(p + 3), out, count);
    if (p[2] >= SET_EQ_FIRST) {
        s32 stored;
        u32 status = set_eq(channel, zone, p[2] - SET_EQ_FIRST, (s32)get_u32(p + 3), &stored);
        if (status != OK) return status;
        put_u32(out, (u32)stored);
        *count = 4;
        return OK;
    }
    if (channel < PRESET_CHANNELS && (p[2] == SET_CHANNEL_MODE || p[2] == SET_MIX_STATE)) {
        s32 value = (s32)get_u32(p + 3);
        u32 status;
        if (p[2] == SET_CHANNEL_MODE) status = set_channel_mode(channel, value);
        else if (value < 0 || value > (s32)CHANNEL_MIX_STATE_SOLO) status = BAD_RANGE;
        else {
            BYTE(CHANNEL_MIX_STATES + channel) = (u8)value;
            channel_mix_rebuild_masks_and_publish((void *)SAMPLE_RUNTIME_OBJECT);
            status = OK;
        }
        if (status != OK) return status;
        put_u32(out, p[2] == SET_CHANNEL_MODE ? channel_record(channel)[CHANNEL_MODE] : BYTE(CHANNEL_MIX_STATES + channel));
        *count = 4;
        return OK;
    }
    if (channel >= PRESET_CHANNELS || zone >= ZONES || !parameter_for(p[2], &parameter)) return BAD_RANGE;
    if (parameter.zone && zone >= active_zones(channel)) return BAD_STATE;
    s32 value = (s32)get_u32(p + 3);
    /* Positions stay within the loaded sample; unbounded remains unbounded. */
    const u8 *sample = parameter.zone ? ready_sample(channel, zone) : 0;
    if (sample && parameter.bytes == 4 && value != (s32)ZONE_POSITION_UNBOUNDED) {
        u32 frames = REG32((u32)sample + SAMPLE_FRAMES);
        parameter.high = (s32)(parameter.offset == ZONE_LOOP_LENGTH ? frames << ZONE_LOOP_LENGTH_SHIFT : frames);
    }
    value = value < parameter.low ? parameter.low : value > parameter.high ? parameter.high : value;
    u8 *record = parameter.zone ? zone_record(channel, zone) : channel_record(channel);
    store_value(record + parameter.offset, parameter.bytes, value);
    if (parameter.zone) preset_sync_stereo_right_channel((void *)PRESET_MODEL, channel, zone);
    publish(channel, zone);
    put_u32(out, (u32)load_value(record + parameter.offset, parameter.bytes));
    *count = 4;
    return OK;
}

/* ---- Automation ---- */

/* A sequencer's lane sets one parameter many times a second. Its message is
 * EDIT_SET with sequence 0, which no request uses, and the value as four
 * nibbles, low first, counted from the parameter's lowest value: every byte
 * is below 80h, so a fixed SysEx template carries it. No reply is sent. The
 * newest value of each parameter waits here and the foreground stores it
 * between jobs, as an edit is stored. */
#define AUTOMATE_BYTES 7u
#define AUTOMATE_SLOTS 16u
#define AUTOMATE_NIBBLE 15u
static struct { volatile u8 pending; u8 channel, zone, id; u16 value; } automated[AUTOMATE_SLOTS];

static void automate_request(const u8 *p, u32 bytes)
{
    if (bytes != AUTOMATE_BYTES) return;
    u32 value = (p[3] & AUTOMATE_NIBBLE) | (p[4] & AUTOMATE_NIBBLE) << 4 |
                (p[5] & AUTOMATE_NIBBLE) << 8 | (p[6] & AUTOMATE_NIBBLE) << 12;
    u32 slot = AUTOMATE_SLOTS;
    for (u32 n = 0; n < AUTOMATE_SLOTS; ++n) {
        if (!automated[n].pending) { if (slot == AUTOMATE_SLOTS) slot = n; continue; }
        if (automated[n].channel == p[0] && automated[n].zone == p[1] && automated[n].id == p[2]) {
            automated[n].value = (u16)value;
            return;
        }
    }
    /* A full table drops the point; the lane's next one takes its place. */
    if (slot == AUTOMATE_SLOTS) return;
    automated[slot].channel = p[0]; automated[slot].zone = p[1]; automated[slot].id = p[2];
    automated[slot].value = (u16)value;
    MEMORY_BARRIER();
    automated[slot].pending = 1;
}

/* Store one automated value: the parameters a lane may move are a channel's
 * and a zone's two-byte values and switches, the times, the EQ's CV routes
 * and the grain values. Sample positions, Channel Mode and Mute/Solo are
 * edits, not automation. */
static void automate_store(u32 channel, u32 zone, u32 id, u32 above)
{
    u8 out[REPLY_BYTES - 1]; u32 count = 0;
    struct parameter parameter;
    if (channel >= PRESET_CHANNELS) return;
    if (id >= SET_RESAMPLE_FIRST && id != SET_GRAIN_FIRST + GRAIN_SET_SLIDE) return;
    if (id >= SET_GRAIN_FIRST) {
        u32 field = id - SET_GRAIN_FIRST;
        if (field >= GRAIN_SET_STATUS && field != GRAIN_SET_SLIDE) return;
        (void)grain_set(channel, field, grain_low(field == GRAIN_SET_SLIDE ? GRAIN_SLIDE : field) + (s32)above, out, &count);
        return;
    }
    if (id >= SET_EQ_FIRST) {
        s32 stored;
        u32 route = id - SET_EQ_FIRST;
        if (route < SET_EQ_SOURCE || route >= SET_EQ_Q) return;
        (void)set_eq(channel, EQ_SLOT_CHANNEL, route,
                     (route < SET_EQ_AMOUNT ? EQ_CV_SOURCE_OFF : -EQ_CV_AMOUNT_MAX) + (s32)above, &stored);
        return;
    }
    if (zone >= ZONES || !parameter_for(id, &parameter)) return;
    if (parameter.zone && (parameter.bytes != 2 || zone >= active_zones(channel))) return;
    s32 value = parameter.low + (s32)above;
    if (value > parameter.high) value = parameter.high;
    u8 *record = parameter.zone ? zone_record(channel, zone) : channel_record(channel);
    store_value(record + parameter.offset, parameter.bytes, value);
    if (parameter.zone) preset_sync_stereo_right_channel((void *)PRESET_MODEL, channel, zone);
    publish(channel, zone);
}

static void automate_foreground(void)
{
    u32 claimed = 0;
    for (u32 n = 0; n < AUTOMATE_SLOTS; ++n) {
        if (!automated[n].pending) continue;
        /* Storage is busy for a moment: the values wait for the next pass. */
        if (!claimed && !storage_claim()) return;
        claimed = 1;
        automated[n].pending = 0;
        MEMORY_BARRIER();
        automate_store(automated[n].channel, automated[n].zone, automated[n].id, automated[n].value);
    }
    if (claimed) storage_release();
}

static u32 write_zone(const u8 *p)
{
    u32 channel = p[0], zone = p[1];
    if (channel >= PRESET_CHANNELS || zone >= ZONES) return BAD_RANGE;
    if (zone >= active_zones(channel)) return BAD_STATE;
    u32 start = get_u32(p + 2), end = get_u32(p + 6), loop = get_u32(p + 10), length = get_u32(p + 14);
    s32 pitch = (s16)get_u16(p + 18), level = (s16)get_u16(p + 20), voltage = (s16)get_u16(p + 22);
    if (start > ZONE_POSITION_UNBOUNDED || end > ZONE_POSITION_UNBOUNDED || loop > ZONE_POSITION_UNBOUNDED ||
        length > ZONE_POSITION_UNBOUNDED || length < ZONE_LOOP_LENGTH_MIN || pitch < -9600 || pitch > 6000 ||
        level < -900 || level > 60 || voltage < -500 || voltage > 500) return BAD_RANGE;
    u8 *z = zone_record(channel, zone);
    put_u32(z + ZONE_SAMPLE_START, start); put_u32(z + ZONE_SAMPLE_END, end);
    put_u32(z + ZONE_LOOP_START, loop); put_u32(z + ZONE_LOOP_LENGTH, length);
    put_u16(z + ZONE_PITCH_OFFSET, (u32)pitch); put_u16(z + ZONE_LEVEL_OFFSET, (u32)level);
    put_u16(z + ZONE_MIN_VOLTAGE, (u32)voltage);
    preset_sync_stereo_right_channel((void *)PRESET_MODEL, channel, zone);
    publish(channel, zone);
    return OK;
}

/* The channel and, when it is this channel's Stereo/Right half, the next:
 * the channels a zone-list change applies to. */
static u32 zone_list_channels(u32 channel)
{
    return channel + 1 < PRESET_CHANNELS && channel_record(channel)[CHANNEL_MODE] != CHANNEL_MODE_STEREO_RIGHT &&
           channel_record(channel + 1)[CHANNEL_MODE] == CHANNEL_MODE_STEREO_RIGHT ? 2 : 1;
}

/* Stock Zones-page Delete: later zones move down a slot, keeping their
 * thresholds, and slot 7 empties; a Stereo/Right successor loses the same
 * zone. */
static void delete_zone(u32 channel, u32 zone)
{
    for (u32 c = channel; c < channel + zone_list_channels(channel); ++c) {
        for (u32 z = zone; z + 1 < ZONES; ++z) memcpy(zone_record(c, z), zone_record(c, z + 1), ZONE_BYTES);
        preset_zone_initialize_empty(zone_record(c, ZONES - 1));
    }
}

/* Remove a zone as the stock Zones page does. */
u32 zone_remove(u32 channel, u32 zone)
{
    u32 zones = active_zones(channel);
    if (channel >= PRESET_CHANNELS || zone >= zones) return BAD_STATE;
    delete_zone(channel, zone);
    rebuild(channel, 0, zones - 1);
    return OK;
}

/* Put a sample in a zone, or in a new zone after the last: one of the active
 * folder, or with a folder prefix the file at `path` in that folder. The
 * stock loader takes a pooled sample of the same name before it reads the
 * card, so another folder's sample of that name refuses the choice. A take
 * plays as it was heard: no zone offsets, and the channel looping. */
u32 zone_assign(u32 channel, u32 zone, const char *name, int take, const char *prefix, const char *path, u32 *frames)
{
    u32 zones = active_zones(channel);
    if (zone >= ZONES || zone > zones) return BAD_STATE;
    u8 *z = zone_record(channel, zone);
    u8 previous[ZONE_BYTES];
    memcpy(previous, z, ZONE_BYTES);
    if (zone == zones) {
        /* A new zone starts as a copy of the one before, like stock Insert. */
        if (zone) memcpy(z, z - ZONE_BYTES, ZONE_BYTES);
        else preset_zone_initialize_empty(z);
    }
    sample_name_store_48((char *)z, name);
    put_u32(z + ZONE_SAMPLE_OBJECT, 0);
    put_u32(z + ZONE_SAMPLE_START, 0); put_u32(z + ZONE_SAMPLE_END, ZONE_POSITION_UNBOUNDED);
    put_u32(z + ZONE_LOOP_START, 0); put_u32(z + ZONE_LOOP_LENGTH, ZONE_POSITION_UNBOUNDED);
    preset_load_missing_sample_data((void *)PRESET_MODEL, (void *)SAMPLE_LOAD_PROGRESS, channel, 0, prefix);
    const u8 *sample = ready_sample(channel, zone);
    int other = sample && prefix && strcmp((const char *)sample + SAMPLE_PATH, path);
    if (!sample || other) {
        memcpy(z, previous, ZONE_BYTES);
        return other ? FILE_EXISTS : IO_ERROR;
    }
    if (zone == zones && zones) preset_channel_balance_zone_voltages(channel_record(channel), ZONE_BALANCE_5V);
    if (take) {
        put_u16(z + ZONE_PITCH_OFFSET, 0); put_u16(z + ZONE_LEVEL_OFFSET, 0);
        z[ZONE_SIDE] = 0;
        if (!channel_record(channel)[CHANNEL_LOOP_MODE]) channel_record(channel)[CHANNEL_LOOP_MODE] = CHANNEL_LOOP_ON;
    }
    rebuild(channel, zone, 1);
    *frames = REG32((u32)sample + SAMPLE_FRAMES);
    return OK;
}

/* Assign a sample to a zone, or append a zone: by name, as the preset holds
 * it (a pooled sample, else the active folder's file), or with
 * ASSIGN_BROWSED the browsed folder's file. An empty name deletes the zone. */
static u32 assign_sample(const u8 *p, u32 bytes, u8 *out, u32 *count)
{
    u32 channel = p[0], zone = p[1] & ~(ASSIGN_BROWSED | ASSIGN_AUDITION), length = bytes - 2, frames, status;
    if (channel >= PRESET_CHANNELS || zone >= ZONES || length >= ZONE_NAME_BYTES) return BAD_RANGE;
    audition_disarm(channel);
    if (!length) { *count = 0; return zone_remove(channel, zone); }
    char name[ZONE_NAME_BYTES], prefix[BROWSE_PREFIX_BYTES], path[SAMPLE_PATH_BYTES];
    memcpy(name, p + 2, length); name[length] = 0;
    for (u32 i = 0; i < length; ++i) if ((u8)name[i] < ' ' || name[i] == '/' || name[i] == '\\' || name[i] == ':') return BAD_RANGE;
    prefix[0] = 0;
    if (p[1] & ASSIGN_BROWSED && (status = browse_source(p + 2, length, prefix, path)) != OK) return status;
    if (p[1] & ASSIGN_AUDITION) channel_stop(channel);
    status = zone_assign(channel, zone, name, 0, prefix[0] ? prefix : 0, path, &frames);
    if (status != OK) return status;
    if (p[1] & ASSIGN_AUDITION) (void)audition_arm(channel, zone);
    put_u32(out, frames);
    *count = 4;
    return OK;
}

/* Replace the channel's zones with slices of one zone's sample. */
static u32 explode(const u8 *p, u32 bytes)
{
    u32 channel = p[0], source = p[1], slices = p[2];
    if (channel >= PRESET_CHANNELS || source >= ZONES || !slices || slices > ZONES ||
        bytes != 3 + (slices + 1) * 4) return BAD_RANGE;
    if (source >= active_zones(channel)) return BAD_STATE;
    const u8 *sample = ready_sample(channel, source);
    if (!sample) return BAD_STATE;
    u32 frames = REG32((u32)sample + SAMPLE_FRAMES), bound[ZONES + 1];
    for (u32 i = 0; i <= slices; ++i) {
        bound[i] = get_u32(p + 3 + i * 4);
        if (bound[i] > frames || (i && bound[i] <= bound[i - 1])) return BAD_RANGE;
    }
    u8 model[ZONE_BYTES];
    memcpy(model, zone_record(channel, source), ZONE_BYTES);
    for (u32 i = 0; i < ZONES; ++i) {
        u8 *z = zone_record(channel, i);
        if (i >= slices) {
            /* A Stereo/Right half keeps no zones past the split either. */
            for (u32 c = channel; c < channel + zone_list_channels(channel); ++c) preset_zone_initialize_empty(zone_record(c, i));
            continue;
        }
        memcpy(z, model, ZONE_BYTES);
        u32 length = (bound[i + 1] - bound[i]) << ZONE_LOOP_LENGTH_SHIFT;
        put_u32(z + ZONE_SAMPLE_START, bound[i]); put_u32(z + ZONE_SAMPLE_END, bound[i + 1]);
        put_u32(z + ZONE_LOOP_START, bound[i]);
        put_u32(z + ZONE_LOOP_LENGTH, length < ZONE_LOOP_LENGTH_MIN ? ZONE_LOOP_LENGTH_MIN : length);
    }
    preset_channel_balance_zone_voltages(channel_record(channel), ZONE_BALANCE_5V);
    rebuild(channel, 0, slices);
    return OK;
}

static u32 transients(const u8 *p, u8 *out, u32 *count)
{
    u32 channel = p[0], zone = p[1], pieces = p[2];
    if (channel >= PRESET_CHANNELS || zone >= ZONES || pieces < 2 || pieces > ZONE_CHOP_MAX_PIECES) return BAD_RANGE;
    const u8 *sample = ready_sample(channel, zone);
    if (!sample) return BAD_STATE;
    u32 stereo = REG32((u32)sample + SAMPLE_CHANNELS) == 2 && REG32((u32)sample + SAMPLE_BUFFER_1);
    zone_chop_detect_transients((void *)ZONE_CHOP_ANALYSIS_STATE, (void *)sample,
                                stereo ? ZONE_CHOP_MODE_STEREO : ZONE_CHOP_MODE_BUFFER_0, pieces);
    const u8 *bound = (const u8 *)ZONE_CHOP_ANALYSIS_STATE + ZONE_CHOP_BOUNDARIES;
    for (u32 i = 0; i <= pieces; ++i) {
        s32 frame = (s32)get_u32(bound + i * 4);
        put_u32(out + i * 4, frame < 0 ? 0 : (u32)frame);
    }
    *count = (pieces + 1) * 4;
    return OK;
}

/* Stop a channel's voice as the panel's Play Mode + channel chord does, so a
 * 1 Shot or looping zone ends: clear its group's software active bits, then
 * the engine voice flags. Note Off alone does not stop a 1 Shot. */
void channel_stop(u32 channel)
{
    u32 group = BYTE(ENGINE_CHANNEL_GROUP_MASKS + channel), mask = group ? group : 1u << channel;
    BYTE(ACTIVE_PRESET_RUNTIME_STATE + RUNTIME_CHANNEL_ACTIVE_MASK) &= (u8)~mask;
    engine_channel_voice_flag_clear_grouped_and_notify(channel);
}

/* An audition armed on the channel ends with its voice. With a zone, that
 * zone is then armed: the channel's next note plays it once. */
static u32 stop_channel(const u8 *p, u32 bytes)
{
    u32 channel = p[0];
    if (channel >= PRESET_CHANNELS || (bytes > 1 && p[1] >= ZONES)) return BAD_RANGE;
    channel_stop(channel);
    audition_disarm(channel);
    return bytes > 1 ? audition_arm(channel, p[1]) : OK;
}

static u32 summary(u8 *out, u32 *count)
{
    out[0] = (u8)REG32(WORKING_PRESET_STATE);
    out[1] = (u8)((preset_working_model_differs_from_library((void *)WORKING_PRESET_STATE) ? SUMMARY_MODIFIED : 0) |
                  (stock_busy() ? SUMMARY_STORAGE_BUSY : 0));
    put_u32(out + 2, ~crc32_update(~0u, (const void *)PRESET_MODEL, PRESET_RECORD_BYTES));
    memcpy(out + 6, (const void *)PRESET_MODEL, PRESET_NAME_BYTES);
    memcpy(out + 18, (const void *)CHANNEL_MIX_STATES, PRESET_CHANNELS);
    u32 n = 0, at = 18 + PRESET_CHANNELS;
    const char *folder = (const char *)ACTIVE_FOLDER_NAME;
    while (n < ACTIVE_FOLDER_BYTES - 1 && folder[n]) { out[at + n] = (u8)folder[n]; ++n; }
    /* The EQ tables' checksum, then grain mode's, after the folder's
     * terminator: older readers stop the folder name there. */
    out[at + n] = 0;
    u32 crc = ~0u;
    crc = crc32_update(crc, eq_channels, sizeof(eq_channels));
    put_u32(out + at + n + 1, ~crc32_update(crc, eq_zones, sizeof(eq_zones)));
    put_u32(out + at + n + 5, grain_crc());
    *count = at + n + 9;
    return OK;
}

u32 sample_info(const u8 *p, u8 *out, u32 *count)
{
    u32 channel = p[0], zone = p[1];
    if (channel >= PRESET_CHANNELS || zone >= ZONES) return BAD_RANGE;
    u32 object = REG32((u32)zone_record(channel, zone) + ZONE_SAMPLE_OBJECT);
    memset(out, 0, 14);
    if (object) {
        put_u32(out, REG32(object + SAMPLE_FRAMES));
        put_u32(out + 4, REG32(object + SAMPLE_RATE));
        out[8] = (u8)REG32(object + SAMPLE_CHANNELS);
        out[9] = BYTE(object + SAMPLE_LOAD_STATE);
        put_u32(out + 10, object);
    }
    *count = 14;
    return OK;
}

/* Save: first the files of samples chosen in other folders are copied into
 * the active folder, a part each pass, as the stock Save does before it
 * writes. Then the stock transaction: the library record is replaced at
 * once and the sample service writes prstNNN.yml. */
static u32 save_index(const u8 *p) { return p[0] ? p[0] : REG32(WORKING_PRESET_STATE); }
static u32 save_start(const u8 *p)
{
    u32 index = save_index(p);
    if (index < PRESET_LIBRARY_FIRST || index > PRESET_LIBRARY_LAST) return BAD_RANGE;
    return stock_busy() ? BUSY : OK;
}

static u32 expected_bytes(u32 command, u32 bytes)
{
    switch (command) {
    case EDIT_SUMMARY: return 0;
    case EDIT_READ: return 3;
    case EDIT_SAMPLE: return 2;
    case EDIT_SET: return 7;
    case EDIT_ZONE: return 24;
    case EDIT_FOLDER: return bytes >= 2 ? bytes : 2;
    case EDIT_PEAKS: return 11;
    case EDIT_TRANSIENTS: return 3;
    case EDIT_SAVE: return 1;
    case EDIT_ASSIGN: return bytes >= 2 ? bytes : 2;
    case EDIT_EXPLODE: return bytes >= 3 ? bytes : 3;
    case EDIT_FILE_PEAKS: return bytes >= 10 ? bytes : 10;
    case EDIT_STOP: return bytes == 2 ? 2 : 1;
    }
    return ~0u;
}

static void foreground(void)
{
    stream_foreground();
    grains_foreground();
    audition_foreground();
    resample_foreground();
    u32 command = job.command;
    if (!command) { automate_foreground(); return; }
    u8 out[REPLY_BYTES - 1]; u32 count = 0, status = OK;
    const u8 *p = job.payload;
    if (job.saving == SAVE_COPYING) {
        /* Storage stays claimed from the request until the files are in. */
        status = externals_copy_step();
        if (status == COPY_MORE) return;
        if (status == OK) preset_save_library_index_schedule_yaml_write((void *)WORKING_PRESET_STATE, save_index(p), 0);
        storage_release();
        job.saving = status == OK ? SAVE_WRITING : SAVE_IDLE; job.started = REG32(TICKS);
        if (status == OK) return;
        goto done;
    }
    if (job.saving) {
        /* The stock service owns the write; report once it returns to idle. */
        if (BYTE(SAMPLE_SERVICE_STATE_ADDR)) {
            if ((u32)(REG32(TICKS) - job.started) < SAVE_TIMEOUT_TICKS) return;
            status = IO_ERROR;
        } else {
            status = BYTE(SAMPLE_SERVICE_RESULT_ADDR) ? IO_ERROR : OK;
            out[0] = (u8)REG32(WORKING_PRESET_STATE); count = 1;
            if (status == OK && (!p[0] || p[0] == out[0])) resample_saved();
        }
        job.saving = SAVE_IDLE;
        goto done;
    }
    if (expected_bytes(command, job.bytes) != job.bytes) { status = BAD_LENGTH; goto done; }
    if (command == EDIT_READ) {
        u32 offset = get_u16(p);
        count = p[2];
        if (!count || count > READ_MAX || offset + count > PRESET_RECORD_BYTES) { status = BAD_RANGE; count = 0; }
        else memcpy(out, (const u8 *)PRESET_MODEL + offset, count);
        goto done;
    }
    if (command == EDIT_SAMPLE) { status = sample_info(p, out, &count); goto done; }
    if (command == EDIT_SUMMARY) { status = summary(out, &count); goto done; }
    if (command == EDIT_STOP) { status = stop_channel(p, job.bytes); goto done; }
    /* What is sounding and what a take is doing are read while a take
     * holds storage. */
    if (command == EDIT_SET && (p[2] == SET_GRAIN_FIRST + GRAIN_SET_STATUS || p[2] == SET_GRAIN_FIRST + GRAIN_SET_VIEW ||
                                p[2] == SET_GRAIN_FIRST + GRAIN_SET_ROUTES ||
                                p[2] == SET_RESAMPLE_FIRST + 1u || p[2] == SET_RESAMPLE_FIRST + 4u)) {
        status = set_parameter(p, out, &count);
        goto done;
    }
    if (command == EDIT_SET && p[2] == SET_RESAMPLE_FIRST) {
        /* Arming claims storage only to name the take: the recorder refuses
         * to arm while anything holds it. */
        status = resample_arm(p[0], p[1], get_u32(p + 3), job.sequence, out, &count);
        if (status == BUSY && !record_busy()) return; /* storage held: retry on the next pass */
        goto done;
    }
    if (!storage_claim()) {
        /* A take holds storage for as long as it runs; other holders are brief. */
        if (!record_busy()) return; /* retry on the next pass */
        status = BUSY;
        goto done;
    }
    switch (command) {
    case EDIT_SET: status = set_parameter(p, out, &count); break;
    case EDIT_ZONE: status = write_zone(p); break;
    case EDIT_ASSIGN: status = assign_sample(p, job.bytes, out, &count); break;
    case EDIT_FOLDER:
        if (job.bytes > 2) status = get_u16(p) == BROWSE_REQUEST ? folder_browse(p + 2, job.bytes - 2, out, &count) : BAD_RANGE;
        else { count = folder_list(get_u16(p), ~0u, out, sizeof(out)); status = count ? OK : IO_ERROR; }
        break;
    case EDIT_PEAKS: status = zone_peaks(p, out, &count); break;
    case EDIT_TRANSIENTS: status = transients(p, out, &count); break;
    case EDIT_EXPLODE: status = explode(p, job.bytes); break;
    case EDIT_FILE_PEAKS: status = file_peaks(p, job.bytes, out, &count); break;
    case EDIT_SAVE:
        status = save_start(p);
        if (status == OK) { job.saving = SAVE_COPYING; return; }
        break;
    default: status = BAD_COMMAND; break;
    }
    storage_release();
done:
    if (status) count = 0;
    reply(command, job.sequence, status, out, count);
    MEMORY_BARRIER();
    job.command = 0;
}

const struct extension_header extension_header __attribute__((section(".extension_header"), used)) = {
    {PATCH_MAGIC_0, EXTENSION_MAGIC}, {0}, 0, 0, 0, init, command, foreground, busy
};
