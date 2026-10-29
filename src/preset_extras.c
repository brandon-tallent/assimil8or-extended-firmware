#include "eq.h"

/* Preset settings beyond the stock model: channel and zone EQ, channel and
 * zone filter, and grain settings.
 *
 * Stock keeps every preset in the folder in a RAM library, parsed from
 * prstNNN.yml when the folder loads. Loading a preset copies its library
 * record to the working model. Save copies it back and rewrites the file.
 * The extras follow the same path in a table beside the library:
 * - parsing fills the preset's slot from the lines below,
 * - loading copies the slot to the live settings,
 * - Save copies the live settings to the slot and appends the lines to the
 *   file after stock's own.
 * Stock firmware ignores these keys on load and drops them on save.
 *
 * Lines (channels and zones numbered from 1):
 *   ExtrasFormat : EXTRAS_FORMAT, always first. Missing means format 1
 *     (builds before 1.0.0).
 *   EQ<c> : on hpf, then frequency gain q for Low, Mid and High.
 *     Frequencies in Hz, as for FILT.
 *   EQCV<c> : source amount for Low frequency, Low gain, Mid frequency,
 *     Mid gain, High frequency, High gain, HPF
 *   EQ<c>Z<z>, EQCV<c>Z<z> : the same for a zone with its own EQ
 *   FILT<c> : type cutoff resonance extra. Cutoff in Hz: whole when that
 *     loads the same step, otherwise two decimals.
 *   FILTCV<c> : source amount for Cutoff, Resonance, Extra, Type
 *   FILT<c>Z<z>, FILTCV<c>Z<z> : the same for a zone with its own filter
 *   FILTR<c>, FILTR<c>Z<z> : stereo pair right-side cutoff offset, in cents
 *     (10 per step). Written only when not 0, after the FILT line.
 *   Grain<c> : on size overlap spray detune octave reverse pan shape scan
 *     slide skew. on is 1 for the channel that plays grains.
 *   GrainCV<c> : source amount for each grain route
 *   TimeLock<c> : on [tempo speed input ppqn]. on is 1 when on. The rest,
 *     written when any is set: tempo 0 channel 1's (on channel 1, Own), 1 Own,
 *     2 Speed, 3 MIDI clock, 4 CV clock;
 *     speed in tenths of a percent from 100%; the clock's CV input selector
 *     (-4 none); the clock's pulses a beat (0: 4).
 *   TimeLock<c>Z<z> : beats. The zone's loop length in beats, written when
 *     set (0: guessed).
 * Format 1 grain lines, read into all eight channels:
 *   Grain : channel size overlap ... skew (channel 0: none plays)
 *   GrainCV : source amount for each grain route
 *
 * The reader accepts every format. The writer writes only the current one.
 * Older builds ignore lines whose number count does not match theirs, so
 * they ignore Grain<c> (one number more than format 1's Grain line).
 * Values are the integers of eq.h and patch.h. Only lines that differ from
 * the defaults are written; a preset with no extras is the stock file. */

#define PRESET_LIBRARY_BASE       0x80116bccu
#define PRESET_RECORD_BYTES       0x1660u   /* PRESET_RECORD_SIZE */
#define PRESET_LIBRARY_RECORDS    200u
#define PRESET_YAML_STATE_MODEL   0x0cu
#define STATUS_BYTE_MASK          0xffu
#define CHANNELS                  8u
#define ZONES                     8u
#define NO_ZONE                   ZONES
#define EQ_NUMBERS                11u       /* on, hpf, three bands of three */
#define FILTER_NUMBERS            4u        /* type, cutoff, resonance, extra */
#define NUMBERS_MAX               16u
/* The numbers that are frequencies, held here in hundredths of a hertz:
 * FILT's cutoff and EQ's three band frequencies. */
#define FILTER_DECIMALS           (1u << 1)
#define EQ_DECIMALS               ((1u << 2) | (1u << 5) | (1u << 8))
#define NO_DECIMALS               0u
#define CENTS_PER_UNIT            100       /* hundredths: a frequency's decimals */
#define CENTS_PER_STEP            10        /* a cutoff step, 1/120 octave */
#define EXTRAS_FORMAT             3         /* per-channel grain, frequencies as steps, FILTR, TimeLock */
#define LINE_BYTES                112u
#define KEY_BYTES                 12u
#define TIME_LOCK_NUMBERS         5u        /* on tempo speed input ppqn */
#define TIME_LOCK_ZONE_NUMBERS    1u        /* beats */

u32 extras_parse_file_stock(u32 model, void *file);
u32 extras_parse_field_stock(u32 *state, const char *line);
u32 extras_save_stock(void *holder, u32 index, void *model);
u32 extras_erase_stock(void *holder, u32 index);
u32 extras_write_stock(void *model, const char *path, u32 ordinal);
u32 extras_differs_stock(void *holder);
int sprintf(char *buffer, const char *format, ...);
int strncasecmp(const char *a, const char *b, u32 bytes);

struct extras patch_live;
u32 patch_grain_loaded;
u32 patch_extras_saved;
/* A slot per library record that has extras. Slots are allocated in the
 * foreground: while a folder's presets are parsed, and one kept ready for a
 * load or Save, which the front panel runs in its interrupt. */
static struct extras *library[PRESET_LIBRARY_RECORDS], *spare;

const struct grain_range grain_ranges[GRAIN_FIELDS] = {
    {0, 1, 0},          /* this channel plays grains */
    {5, 20000, 600},    /* size, 0.1 ms */
    {25, 800, 400},     /* overlap, percent: grains sounding at once x100 */
    {0, 2000, 20},      /* spray, ms of random position */
    {0, 1200, 0},       /* detune, cents either way */
    {0, 100, 0},        /* grains an octave up, percent */
    {0, 100, 0},        /* grains played backwards, percent */
    {0, 100, 50},       /* pan spread, percent (stereo pair) */
    {1, 100, 100},      /* shape: percent of the grain spent fading */
    {-400, 400, 100},   /* scan, percent of the voice's speed */
    {-1000, 1000, 0},   /* slide, tenths of a percent of the zone added to the start */
    {-100, 100, 0},     /* skew: -100 all release (Decay), +100 all attack (Reverse) */
};

static void defaults(struct extras *e)
{
    memset(e, 0, sizeof(*e));
    for (u32 ch = 0; ch < CHANNELS; ++ch) {
        eq_default(&e->channels[ch]);
        filter_default(&e->filters[ch], ch);
    }
    for (u32 ch = 0; ch < CHANNELS; ++ch) {
        for (u32 f = 0; f < GRAIN_FIELDS; ++f) e->grain[ch].value[f] = grain_ranges[f].initial;
        for (u32 r = 0; r < GRAIN_ROUTES; ++r) {
            e->grain[ch].source[r] = EQ_CV_SOURCE_OFF;
            e->grain[ch].amount[r] = EQ_CV_AMOUNT_MAX;
        }
    }
}

/* The live settings become a slot's, or the defaults. */
static void load(const struct extras *e)
{
    if (e) patch_live = *e; else defaults(&patch_live);
    eq_panel_forget();
    eq_changed();
    filter_changed();
    patch_grain_loaded++;
}
void preset_extras_reset(void) { load(0); }
void preset_extras_foreground(void)
{
    if (!spare) spare = malloc(sizeof(*spare));
}

/* Foreground only. */
static void forget(u32 index)
{
    if (index >= PRESET_LIBRARY_RECORDS || !library[index]) return;
    struct extras *e = library[index];
    library[index] = 0;
    free(e);
}

/* A library record's slot, made from the ready one when it has none. */
static struct extras *slot(u32 index)
{
    if (index >= PRESET_LIBRARY_RECORDS) return 0;
    if (!library[index] && spare) {
        defaults(spare);
        library[index] = spare;
        spare = 0;
    }
    return library[index];
}

/* Where a parsed model's extras go: the live settings for the working
 * model, a slot for a library record, nowhere for a temporary one. Library
 * records are parsed in the foreground. */
static struct extras *destination(u32 model)
{
    if (model == EQ_MODEL) return &patch_live;
    u32 index = (model - PRESET_LIBRARY_BASE) / PRESET_RECORD_BYTES;
    if (index >= PRESET_LIBRARY_RECORDS) return 0;
    if (!library[index] && !spare) spare = malloc(sizeof(*spare));
    return slot(index);
}

static s32 clamp(s32 x, s32 low, s32 high) { return x < low ? low : x > high ? high : x; }

static void put_routes(signed char *source, signed char *amount, const s32 *n, u32 routes)
{
    for (u32 r = 0; r < routes; ++r) {
        source[r] = (signed char)clamp(n[2*r], EQ_CV_SOURCE_OFF, EQ_CV_SOURCE_MAX);
        amount[r] = (signed char)clamp(n[2*r+1], -EQ_CV_AMOUNT_MAX, EQ_CV_AMOUNT_MAX);
    }
}
static u32 get_routes(const signed char *source, const signed char *amount, s32 *n, u32 routes)
{
    u32 set = 0;
    for (u32 r = 0; r < routes; ++r) {
        n[2*r] = source[r]; n[2*r+1] = amount[r];
        set |= source[r] != EQ_CV_SOURCE_OFF || amount[r] != EQ_CV_AMOUNT_MAX;
    }
    return set;
}
static void put_eq(struct eq_value *v, const s32 *n)
{
    v->on = n[0] != 0;
    v->hpf = (u16)(n[1] ? clamp(n[1], EQ_HPF_MIN, EQ_HPF_MAX) : 0);
    for (u32 band = 0; band < 3; ++band) {
        v->frequency[band] = (u16)eq_step_nearest((u32)clamp(n[2+3*band], 0, EQ_FREQUENCY_MAX*CENTS_PER_UNIT));
        v->gain[band] = (signed short)clamp(n[3+3*band], -EQ_GAIN_MAX, EQ_GAIN_MAX);
        v->q[band] = (u8)clamp(n[4+3*band], EQ_Q_OWN, EQ_Q_STEPS);
    }
    eq_limit(v);
}
static void get_eq(const struct eq_value *v, s32 *n)
{
    n[0] = v->on; n[1] = v->hpf;
    for (u32 band = 0; band < 3; ++band) {
        n[2+3*band] = (s32)eq_step_centihertz(v->frequency[band]); n[3+3*band] = v->gain[band]; n[4+3*band] = v->q[band];
    }
}

static void put_filter(struct filter_value *v, const s32 *n)
{
    v->type = (u8)clamp(n[0], FILTER_OFF, FILTER_TYPES - 1);
    v->cutoff = (u16)eq_step_nearest((u32)clamp(n[1], 0, EQ_FREQUENCY_MAX*CENTS_PER_UNIT));
    v->resonance = (u8)clamp(n[2], 0, FILTER_RESONANCE_MAX);
    v->extra = (u8)clamp(n[3], 0, FILTER_EXTRA_MAX);
}
static void get_filter(const struct filter_value *v, s32 *n)
{
    n[0] = v->type; n[1] = (s32)eq_step_centihertz(v->cutoff);
    n[2] = v->resonance; n[3] = v->extra;
}

/* A signed decimal after spaces and the key's colon; null when there is none. */
static const char *number(const char *s, s32 *out)
{
    while (*s == ' ' || *s == '\t' || *s == ':') ++s;
    u32 negative = *s == '-';
    if (negative) ++s;
    if (*s < '0' || *s > '9') return 0;
    s32 value = 0;
    while (*s >= '0' && *s <= '9') value = value*10 + (*s++ - '0');
    *out = negative ? -value : value;
    return s;
}
/* A decimal with up to two places (more are dropped), in hundredths. */
static const char *hundredths(const char *s, s32 *out)
{
    s32 whole;
    if (!(s = number(s, &whole))) return 0;
    s32 fraction = 0, scale = CENTS_PER_UNIT;
    if (*s == '.')
        for (++s; *s >= '0' && *s <= '9'; ++s)
            if (scale /= 10) fraction += (*s - '0')*scale;
    *out = whole*CENTS_PER_UNIT + (whole < 0 ? -fraction : fraction);
    return s;
}

/* One line of a preset file: store it when it is one of ours and complete. */
static void line(u32 model, const char *s)
{
    while (*s == ' ' || *s == '\t') ++s;
    if (!strncasecmp(s, "TimeLock", 8)) {
        s += 8;
        u32 channel = (u32)(*s++ - '1'), zone = NO_ZONE;
        if (channel >= CHANNELS) return;
        if (*s == 'Z' || *s == 'z') { zone = (u32)(s[1] - '1'); s += 2; if (zone >= ZONES) return; }
        /* The numbers after the first are optional; absent ones are the defaults. */
        s32 n[TIME_LOCK_NUMBERS] = {0, 0, 0, EQ_CV_SOURCE_OFF, 0}, more;
        u32 count = 0, wanted = zone == NO_ZONE ? TIME_LOCK_NUMBERS : TIME_LOCK_ZONE_NUMBERS;
        for (const char *next; count < wanted && (next = number(s, &n[count])); s = next) ++count;
        struct extras *e = destination(model);
        if (!count || number(s, &more) || !e) return;
        if (zone == NO_ZONE) {
            e->time_lock[channel] = n[0] != 0;
            e->tempo[channel] = (u8)clamp(n[1], 0, TIME_LOCK_TEMPOS - 1);
            e->tempo_speed[channel] = (s16)clamp(n[2], TIME_LOCK_SPEED_MIN, TIME_LOCK_SPEED_MAX);
            e->tempo_input[channel] = (u8)(clamp(n[3], EQ_CV_SOURCE_OFF, EQ_CV_SOURCE_MAX) - EQ_CV_SOURCE_OFF);
            e->tempo_ppqn[channel] = (u8)clamp(n[4], 0, TIME_LOCK_PPQN_MAX);
        } else e->beats[channel][zone] = (u8)clamp(n[0], 0, TIME_LOCK_BEATS_MAX);
        return;
    }
    u32 grain = !strncasecmp(s, "Grain", 5), filter = !strncasecmp(s, "FILT", 4);
    if (!grain && !filter && strncasecmp(s, "EQ", 2)) return;
    s += grain ? 5 : filter ? 4 : 2;
    u32 cv = !strncasecmp(s, "CV", 2), channel = 0, zone = NO_ZONE;
    if (cv) s += 2;
    u32 right = filter && !cv && (*s == 'R' || *s == 'r');
    if (right) ++s;
    /* "Grain" with no channel number: format 1, one set for all eight channels. */
    u32 every = grain && (*s < '1' || *s > '9');
    if (!every) {
        channel = (u32)(*s++ - '1');
        if (channel >= CHANNELS) return;
        if (!grain && (*s == 'Z' || *s == 'z')) {
            zone = (u32)(s[1] - '1');
            if (zone >= ZONES) return;
            s += 2;
        }
    }
    u32 wanted = right ? 1 : cv ? 2*(grain ? GRAIN_ROUTES : filter ? FILTER_MOD_COUNT : EQ_MOD_COUNT)
                    : grain ? GRAIN_FIELDS : filter ? FILTER_NUMBERS : EQ_NUMBERS, count = 0;
    s32 n[NUMBERS_MAX];
    u32 decimals = cv || grain ? NO_DECIMALS : filter ? FILTER_DECIMALS : EQ_DECIMALS;
    while (count < wanted &&
           (s = decimals & (1u << count) ? hundredths(s, &n[count]) : number(s, &n[count]))) ++count;
    /* A line with more or fewer numbers than ours has another layout. */
    s32 more;
    if (count != wanted || number(s, &more)) return;
    struct extras *e = destination(model);
    if (!e) return;
    if (grain) {
        for (u32 ch = every ? 0 : channel; ch < (every ? CHANNELS : channel + 1); ++ch) {
            struct grain_settings *g = &e->grain[ch];
            if (cv) { put_routes(g->source, g->amount, n, GRAIN_ROUTES); continue; }
            for (u32 f = 0; f < GRAIN_FIELDS; ++f)
                g->value[f] = (s16)clamp(n[f], grain_ranges[f].low, grain_ranges[f].high);
            /* In format 1 the first number is the playing channel (0: none). */
            if (every) g->value[GRAIN_ON] = n[GRAIN_ON] == (s32)ch + 1;
        }
        return;
    }
    if (filter) {
        struct filter_value *f = zone == NO_ZONE ? &e->filters[channel] : &e->zone_filters[channel][zone];
        if (zone != NO_ZONE && !f->override) { filter_default(f, channel); f->override = 1; }
        if (right) {
            s32 cents = clamp(n[0], -(s32)EQ_STEP_MAX*CENTS_PER_STEP, (s32)EQ_STEP_MAX*CENTS_PER_STEP);
            f->right = (s16)((cents + (cents < 0 ? -CENTS_PER_STEP/2 : CENTS_PER_STEP/2))/CENTS_PER_STEP);
        } else if (cv) put_routes(f->source, f->amount, n, FILTER_MOD_COUNT); else put_filter(f, n);
        return;
    }
    struct eq_value *v = zone == NO_ZONE ? &e->channels[channel] : &e->zones[channel][zone];
    if (zone != NO_ZONE && !v->override) { eq_default(v); v->override = 1; }
    if (cv) put_routes(v->source, v->amount, n, EQ_MOD_COUNT); else put_eq(v, n);
}

/* `decimals` marks the numbers held in hundredths of a hertz. One is
 * written whole when that loads the same step, so files older firmware can
 * read stay that way where they can. */
static u32 format_written;  /* the ExtrasFormat line has been written to this file */
static void write_line(FIL *file, const char *key, const s32 *n, u32 count, u32 decimals)
{
    char text[LINE_BYTES];
    u32 done, at;
    if (!format_written) {
        format_written = 1;
        at = (u32)sprintf(text, "  ExtrasFormat : %d\n", EXTRAS_FORMAT);
        resource_write_bytes(file, text, at, &done);
    }
    at = (u32)sprintf(text, "  %s :", key);
    for (u32 i = 0; i < count; ++i) {
        s32 whole = (n[i] + CENTS_PER_UNIT/2)/CENTS_PER_UNIT;
        if (!(decimals & (1u << i))) at += (u32)sprintf(text + at, " %d", n[i]);
        else if (eq_step_nearest((u32)(whole*CENTS_PER_UNIT)) == eq_step_nearest((u32)n[i]))
            at += (u32)sprintf(text + at, " %d", whole);
        else at += (u32)sprintf(text + at, " %d.%02d", n[i]/CENTS_PER_UNIT, n[i]%CENTS_PER_UNIT);
    }
    text[at++] = '\n';
    resource_write_bytes(file, text, at, &done);
}

/* One EQ record's lines. A channel's are written when they differ from the
 * defaults; a zone with its own EQ always has its first. */
static void write_eq(FIL *file, const struct eq_value *v, u32 channel, u32 zone)
{
    struct eq_value plain;
    char key[KEY_BYTES], suffix[4] = "";
    s32 n[NUMBERS_MAX], standard[EQ_NUMBERS];
    eq_default(&plain);
    get_eq(&plain, standard);
    get_eq(v, n);
    if (zone != NO_ZONE) sprintf(suffix, "Z%d", zone + 1);
    if (zone != NO_ZONE || memcmp(n, standard, sizeof(standard))) {
        sprintf(key, "EQ%d%s", channel + 1, suffix);
        write_line(file, key, n, EQ_NUMBERS, EQ_DECIMALS);
    }
    if (get_routes(v->source, v->amount, n, EQ_MOD_COUNT)) {
        sprintf(key, "EQCV%d%s", channel + 1, suffix);
        write_line(file, key, n, 2*EQ_MOD_COUNT, NO_DECIMALS);
    }
}

/* One filter record's lines, as an EQ record's. */
static void write_filter(FIL *file, const struct filter_value *v, u32 channel, u32 zone)
{
    struct filter_value plain;
    char key[KEY_BYTES], suffix[4] = "";
    s32 n[NUMBERS_MAX], standard[FILTER_NUMBERS];
    filter_default(&plain, channel);
    get_filter(&plain, standard);
    get_filter(v, n);
    if (zone != NO_ZONE) sprintf(suffix, "Z%d", zone + 1);
    if (zone != NO_ZONE || memcmp(n, standard, sizeof(standard))) {
        sprintf(key, "FILT%d%s", channel + 1, suffix);
        write_line(file, key, n, FILTER_NUMBERS, FILTER_DECIMALS);
    }
    if (get_routes(v->source, v->amount, n, FILTER_MOD_COUNT)) {
        sprintf(key, "FILTCV%d%s", channel + 1, suffix);
        write_line(file, key, n, 2*FILTER_MOD_COUNT, NO_DECIMALS);
    }
    if (v->right) {
        n[0] = v->right*CENTS_PER_STEP;
        sprintf(key, "FILTR%d%s", channel + 1, suffix);
        write_line(file, key, n, 1, NO_DECIMALS);
    }
}

static void append(const char *path, const struct extras *e)
{
    FIL file;
    s32 n[NUMBERS_MAX];
    if (fatfs_open(&file, path, FA_WRITE)) return;
    format_written = 0;
    if (!resource_seek_absolute(&file, file.words[FIL_FILE_SIZE / 4])) {
        for (u32 ch = 0; ch < CHANNELS; ++ch) {
            write_eq(&file, &e->channels[ch], ch, NO_ZONE);
            for (u32 z = 0; z < ZONES; ++z)
                if (e->zones[ch][z].override) write_eq(&file, &e->zones[ch][z], ch, z);
            write_filter(&file, &e->filters[ch], ch, NO_ZONE);
            for (u32 z = 0; z < ZONES; ++z)
                if (e->zone_filters[ch][z].override) write_filter(&file, &e->zone_filters[ch][z], ch, z);
        }
        for (u32 ch = 0; ch < CHANNELS; ++ch) {
            const struct grain_settings *g = &e->grain[ch];
            char key[KEY_BYTES];
            u32 differs = 0;
            for (u32 f = 0; f < GRAIN_FIELDS; ++f) {
                n[f] = g->value[f];
                differs |= n[f] != grain_ranges[f].initial;
            }
            sprintf(key, "Grain%d", ch + 1);
            if (differs) write_line(&file, key, n, GRAIN_FIELDS, NO_DECIMALS);
            sprintf(key, "GrainCV%d", ch + 1);
            if (get_routes(g->source, g->amount, n, GRAIN_ROUTES)) write_line(&file, key, n, 2*GRAIN_ROUTES, NO_DECIMALS);
        }
        for (u32 ch = 0; ch < CHANNELS; ++ch) {
            char key[KEY_BYTES];
            n[0] = e->time_lock[ch]; n[1] = e->tempo[ch]; n[2] = e->tempo_speed[ch];
            n[3] = e->tempo_input[ch] + EQ_CV_SOURCE_OFF; n[4] = e->tempo_ppqn[ch];
            u32 tempo = n[1] || n[2] || e->tempo_input[ch] || n[4];
            sprintf(key, "TimeLock%d", ch + 1);
            if (n[0] || tempo) write_line(&file, key, n, tempo ? TIME_LOCK_NUMBERS : 1, NO_DECIMALS);
            for (u32 z = 0; z < ZONES; ++z) {
                if (!(n[0] = e->beats[ch][z])) continue;
                sprintf(key, "TimeLock%dZ%d", ch + 1, z + 1);
                write_line(&file, key, n, TIME_LOCK_ZONE_NUMBERS, NO_DECIMALS);
            }
        }
    }
    resource_close(&file);
}

/* The hooks: each stands in for the stock routine named in its comment and
 * calls it through the replay stub in entry.S. */

/* preset_yaml_parse_open_file: the model starts from no extras. */
u32 preset_extras_parse_file(u32 model, void *file)
{
    if (model == EQ_MODEL) load(0);
    else forget((model - PRESET_LIBRARY_BASE) / PRESET_RECORD_BYTES);
    return extras_parse_file_stock(model, file);
}
/* preset_yaml_parse_field */
u32 preset_extras_parse_field(u32 *state, const char *text)
{
    line(state[PRESET_YAML_STATE_MODEL / 4], text);
    return extras_parse_field_stock(state, text);
}
/* preset_library_load_yaml_folder, before it: another folder's presets. */
void preset_extras_folder(void)
{
    for (u32 index = 0; index < PRESET_LIBRARY_RECORDS; ++index) forget(index);
}
/* preset_load_library_index_into_working_model, after it. */
void preset_extras_loaded(u32 index)
{
    if (index - 1 < PRESET_LIBRARY_RECORDS - 1) load(slot(index));
}
/* preset_save_library_index_schedule_yaml_write: the slot takes the live
 * settings, as the library record takes the model. */
u32 preset_extras_save(void *holder, u32 index, void *model)
{
    struct extras *e = slot(index);
    if (e) *e = patch_live;
    eq_panel_forget();
    patch_extras_saved++;
    return extras_save_stock(holder, index, model);
}
/* preset_erase_library_index_schedule_yaml_delete; its caller loads the
 * record again afterwards. */
u32 preset_extras_erase(void *holder, u32 index)
{
    if (index < PRESET_LIBRARY_RECORDS && library[index]) defaults(library[index]);
    return extras_erase_stock(holder, index);
}
/* preset_yaml_write_file */
u32 preset_extras_write(void *model, const char *path, u32 ordinal)
{
    u32 status = extras_write_stock(model, path, ordinal);
    if (!(status & STATUS_BYTE_MASK) && ordinal < PRESET_LIBRARY_RECORDS && library[ordinal]) append(path, library[ordinal]);
    return status;
}
/* preset_working_model_differs_from_library: unsaved extras count too. */
u32 preset_extras_differs(u32 *holder)
{
    u32 differs = extras_differs_stock(holder), index = holder[0];
    if (differs || index >= PRESET_LIBRARY_RECORDS || !library[index]) return differs;
    return memcmp(&patch_live, library[index], sizeof(patch_live)) != 0;
}
