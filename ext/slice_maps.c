#include "ext.h"

/* Time Lock's slice maps: each played sample's hits, kept while a zone uses it. */

#define SLICE_MAPS            (PRESET_CHANNELS * ZONES)
#define SLICE_MAP_POSITIONS   512u       /* about 25 s of a busy loop; past that, one slice */
#define ANALYSIS_PASS_CYCLES  2000000u   /* 2 ms of each foreground pass */
#define SAMPLE_STEREO         2u
#define SLICE_MIN_MS          30u        /* a hit this close after the last kept one starts no slice */
#define MS_PER_SECOND         1000u

enum { MAP_FREE, MAP_WAITING, MAP_READY };

struct slice_map {
    const u8 *sample;
    const s32 *frame0;
    u32 frames, analysed, count;
    u8 state, used;
};

struct slice_map_stats {
    u32 analyses, frames, rate, count, hits, cycles, busy_cycles, passes, pass_cycles_max;
};

struct slice_map slice_maps[SLICE_MAPS];
u32 slice_map_positions[SLICE_MAPS][SLICE_MAP_POSITIONS];
struct slice_map_stats slice_map_stats;
static s32 analysing = -1;
static u32 started, busy, passes, pass_max;


static const s32 *first_frame(const u8 *sample, u32 buffer)
{
    u32 address = REG32((u32)sample + SAMPLE_BUFFER_0 + buffer * 4u);
    return address ? (const s32 *)address + SAMPLE_GUARD_FRAMES : 0;
}

static void claim(const u8 *sample)
{
    const s32 *frame0 = first_frame(sample, 0);
    u32 frames = REG32((u32)sample + SAMPLE_FRAMES), free = SLICE_MAPS;
    if (!frame0) return;
    for (u32 m = 0; m < SLICE_MAPS; ++m) {
        struct slice_map *map = &slice_maps[m];
        if (map->state == MAP_FREE) { if (free == SLICE_MAPS) free = m; continue; }
        if (map->sample == sample && map->frame0 == frame0 && map->frames == frames) { map->used = 1; return; }
    }
    if (free == SLICE_MAPS) return;
    struct slice_map *map = &slice_maps[free];
    map->sample = sample; map->frame0 = frame0; map->frames = frames;
    map->analysed = map->count = 0;
    map->used = 1;
    MEMORY_BARRIER();
    map->state = MAP_WAITING;
}

static void start(u32 m)
{
    const struct slice_map *map = &slice_maps[m];
    const s32 *right = REG32((u32)map->sample + SAMPLE_CHANNELS) == SAMPLE_STEREO ? first_frame(map->sample, 1) : 0;
    hits_begin(map->frame0, right, map->frames, REG32((u32)map->sample + SAMPLE_RATE));
    analysing = (s32)m;
    started = cpu_cycles();
    busy = passes = pass_max = 0;
}

static void finish(u32 m)
{
    struct slice_map *map = &slice_maps[m];
    map->count = hits_take(slice_map_positions[m], SLICE_MAP_POSITIONS, &map->analysed);
    /* A hit too soon after the one before stays a rise. */
    u32 *p = slice_map_positions[m], kept = 0, gap = REG32((u32)map->sample + SAMPLE_RATE) * SLICE_MIN_MS / MS_PER_SECOND;
    int any = 0;
    for (u32 i = 0; i < map->count; ++i) {
        if (!(p[i] & HIT_FLAG)) continue;
        u32 at = p[i] & ~HIT_FLAG;
        if (any && at - kept < gap) p[i] = at;
        else { kept = at; any = 1; }
    }
    MEMORY_BARRIER();
    map->state = MAP_READY;
    analysing = -1;
    struct slice_map_stats *st = &slice_map_stats;
    st->analyses++;
    st->frames = map->frames;
    st->rate = REG32((u32)map->sample + SAMPLE_RATE);
    st->count = map->count;
    st->hits = 0;
    for (u32 i = 0; i < map->count; ++i) st->hits += slice_map_positions[m][i] >> 31;
    st->cycles = cpu_cycles() - started;
    st->busy_cycles = busy;
    st->passes = passes;
    st->pass_cycles_max = pass_max;
}

void slice_maps_foreground(void)
{
    for (u32 m = 0; m < SLICE_MAPS; ++m) slice_maps[m].used = 0;
    for (u32 c = 0; c < PRESET_CHANNELS; ++c)
        for (u32 z = 0; z < ZONES; ++z) {
            const u8 *sample = zone_record(c, z)[0] ? ready_sample(c, z) : 0;
            if (sample && timelock_wanted(c)) claim(sample); /* the channels with Time Lock on */
        }
    for (u32 m = 0; m < SLICE_MAPS; ++m) {
        struct slice_map *map = &slice_maps[m];
        if (map->state == MAP_FREE || map->used) continue;
        map->state = MAP_FREE;
        if (analysing == (s32)m) analysing = -1;
    }
    if (analysing < 0)
        for (u32 m = 0; m < SLICE_MAPS; ++m)
            if (slice_maps[m].state == MAP_WAITING) { start(m); break; }
    /* the stock sample service loads and saves in the foreground too */
    if (analysing < 0 || BYTE(SAMPLE_SERVICE_STATE_ADDR)) return;
    u32 begin = cpu_cycles(), spent;
    int done;
    do {
        done = hits_step();
        spent = cpu_cycles() - begin;
    } while (!done && spent < ANALYSIS_PASS_CYCLES);
    busy += spent; ++passes;
    if (spent > pass_max) pass_max = spent;
    if (done) finish((u32)analysing);
}

const u32 *slice_map_find(const u8 *sample, u32 *count, u32 *analysed)
{
    for (u32 m = 0; m < SLICE_MAPS; ++m) {
        const struct slice_map *map = &slice_maps[m];
        if (map->state != MAP_READY || map->sample != sample) continue;
        *count = map->count; *analysed = map->analysed;
        return slice_map_positions[m];
    }
    return 0;
}
