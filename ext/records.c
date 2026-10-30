#include "ext.h"
#include "mix_fx.h"

/* The extras (EQ, filter, grain, Time Lock, compressor and sends) sit beside the stock records, so
 * a stock copy of a channel or zone moves only the stock part. The overlay
 * reports each stock record copy and zone clear (Copy Channel, Copy Zone,
 * Explode, Chop, Clone, zone delete, flip and split), and this moves the
 * extras the same way. A copy to a record outside the working preset (a
 * snapshot or scratch record) keeps its extras here, by address, for the
 * copy back. Grain On stays with the destination: one channel plays grains. */

#define HELD_RECORDS 16u

struct zone_extras { struct eq_value eq; struct filter_value filter; u8 beats; };
struct channel_extras {
    struct eq_value eq;
    struct filter_value filter;
    struct grain_settings grain;
    u8 time_lock, tempo, tempo_input, tempo_ppqn;
    s16 tempo_speed;
    struct zone_extras zones[ZONES];
    struct mix_channel mix;
};
static struct { const u8 *at; u32 bytes; struct channel_extras extras; } held[HELD_RECORDS];
static u32 next_held;
u32 records_copied_count;

static void zone_get(const struct extras *e, u32 c, u32 z, struct zone_extras *out)
{
    if (!e) { memset(out, 0, sizeof(*out)); return; }
    out->eq = e->zones[c][z];
    out->filter = e->zone_filters[c][z];
    out->beats = e->beats[c][z];
}
static void channel_get(const struct extras *e, u32 c, struct channel_extras *out)
{
    memset(out, 0, sizeof(*out));
    const struct mix_settings *m = e ? (const struct mix_settings *)e->extension : 0;
    mix_channel_get(m && m->format == MIX_FORMAT ? m : &mix_defaults, c, &out->mix);
    if (!e) {
        eq_default(&out->eq);
        filter_default(&out->filter, c);
        for (u32 f = 0; f < GRAIN_FIELDS; ++f) out->grain.value[f] = grain_ranges[f].initial;
        for (u32 r = 0; r < GRAIN_ROUTES; ++r) {
            out->grain.source[r] = EQ_CV_SOURCE_OFF;
            out->grain.amount[r] = EQ_CV_AMOUNT_MAX;
        }
        return;
    }
    out->eq = e->channels[c];
    out->filter = e->filters[c];
    out->grain = e->grain[c];
    out->time_lock = e->time_lock[c];
    out->tempo = e->tempo[c];
    out->tempo_input = e->tempo_input[c];
    out->tempo_ppqn = e->tempo_ppqn[c];
    out->tempo_speed = e->tempo_speed[c];
    for (u32 z = 0; z < ZONES; ++z) zone_get(e, c, z, &out->zones[z]);
}
static void zone_put(u32 c, u32 z, const struct zone_extras *in)
{
    eq_zones[c][z] = in->eq;
    filter_zones[c][z] = in->filter;
    time_lock_beats[c][z] = in->beats;
}
static void channel_put(u32 c, const struct channel_extras *in)
{
    s16 on = patch_grains[c].value[GRAIN_ON];
    eq_channels[c] = in->eq;
    filter_channels[c] = in->filter;
    patch_grains[c] = in->grain;
    patch_grains[c].value[GRAIN_ON] = on;
    time_lock_channels[c] = in->time_lock;
    time_lock_tempo[c] = in->tempo;
    time_lock_input[c] = in->tempo_input;
    time_lock_ppqn[c] = in->tempo_ppqn;
    time_lock_speed[c] = in->tempo_speed;
    for (u32 z = 0; z < ZONES; ++z) zone_put(c, z, &in->zones[z]);
    mix_channel_put(c, &in->mix);
}

/* Where a channel record (bytes PRESET_CHANNEL_BYTES) or zone record
 * (ZONE_BYTES) lies in a preset model: its channel and zone. */
static int in_model(u32 at, u32 model, u32 bytes, u32 *channel, u32 *zone)
{
    u32 start = model + PRESET_CHANNELS_OFFSET, offset = at - start;
    if (at < start || offset >= PRESET_CHANNELS * PRESET_CHANNEL_BYTES) return 0;
    u32 within = offset % PRESET_CHANNEL_BYTES;
    *channel = offset / PRESET_CHANNEL_BYTES;
    *zone = within / ZONE_BYTES;
    if (bytes == PRESET_CHANNEL_BYTES) return !within;
    return !(within % ZONE_BYTES) && *zone < ZONES;
}
/* A record's extras, read into a channel's (a zone's in zones[0]). */
static int read_channel(u32 at, struct channel_extras *out)
{
    u32 c, z;
    if (in_model(at, PRESET_MODEL, PRESET_CHANNEL_BYTES, &c, &z)) { channel_get(&patch_live, c, out); return 1; }
    u32 index = (at - PRESET_LIBRARY_BASE) / PRESET_RECORD_BYTES;
    if (at >= PRESET_LIBRARY_BASE && index <= PRESET_LIBRARY_LAST
        && in_model(at, PRESET_LIBRARY_BASE + index * PRESET_RECORD_BYTES, PRESET_CHANNEL_BYTES, &c, &z)) {
        channel_get(preset_extras_slot(index), c, out);
        return 1;
    }
    for (u32 h = 0; h < HELD_RECORDS; ++h)
        if (held[h].at == (const u8 *)at && held[h].bytes == PRESET_CHANNEL_BYTES) { *out = held[h].extras; return 1; }
    return 0;
}
static int read_zone(u32 at, struct zone_extras *out)
{
    u32 c, z;
    if (in_model(at, PRESET_MODEL, ZONE_BYTES, &c, &z)) { zone_get(&patch_live, c, z, out); return 1; }
    for (u32 h = 0; h < HELD_RECORDS; ++h) {
        if (!held[h].at) continue;
        u32 offset = at - (u32)held[h].at;
        if (held[h].bytes == ZONE_BYTES && !offset) { *out = held[h].extras.zones[0]; return 1; }
        if (held[h].bytes == PRESET_CHANNEL_BYTES && offset < ZONES * ZONE_BYTES && !(offset % ZONE_BYTES)) {
            *out = held[h].extras.zones[offset / ZONE_BYTES];
            return 1;
        }
    }
    return 0;
}
static struct channel_extras *hold(const void *at, u32 bytes)
{
    u32 h = 0;
    while (h < HELD_RECORDS && held[h].at != at) ++h;
    if (h == HELD_RECORDS) { h = next_held; next_held = (next_held + 1) % HELD_RECORDS; }
    held[h].at = at;
    held[h].bytes = bytes;
    return &held[h].extras;
}

static void copied(void *to, const void *from, u32 bytes)
{
    u32 c, z, changed = 0;
    if (bytes == PRESET_RECORD_BYTES) {
        if ((u32)from != PRESET_MODEL) return;
        for (c = 0; c < PRESET_CHANNELS; ++c)
            channel_get(&patch_live, c, hold((u8 *)to + PRESET_CHANNELS_OFFSET + c * PRESET_CHANNEL_BYTES, PRESET_CHANNEL_BYTES));
        return;
    }
    if (bytes == PRESET_CHANNEL_BYTES) {
        struct channel_extras e;
        if (!read_channel((u32)from, &e)) return;
        if (in_model((u32)to, PRESET_MODEL, bytes, &c, &z)) { channel_put(c, &e); changed = 1; }
        else *hold(to, bytes) = e;
    } else if (bytes == ZONE_BYTES) {
        struct zone_extras e;
        if (!from) memset(&e, 0, sizeof(e));
        else if (!read_zone((u32)from, &e)) return;
        if (in_model((u32)to, PRESET_MODEL, bytes, &c, &z)) { zone_put(c, z, &e); changed = 1; }
        else hold(to, bytes)->zones[0] = e;
    }
    if (!changed) return;
    eq_changed();
    filter_changed();
    records_copied_count++;
}

void records_init(void) { patch_record_copied = copied; }
