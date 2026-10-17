#include "eq.h"

/* What a preset keeps beyond the stock model: the channel and zone EQ and
 * the grain settings.
 *
 * The stock firmware holds every preset of the folder in a RAM library,
 * parsed from prstNNN.yml when the folder loads. A preset load copies a
 * library record into the working model; Save copies it back and rewrites
 * the file. The extras take the same route in a table beside the library: a
 * slot is filled from the lines below as the stock parser reads them, copied
 * to the live settings on a load and from them on Save, and appended to the
 * file after the stock writer. Stock firmware ignores the lines (their keys
 * are unknown to it) and drops them when it saves.
 *
 *   EQ<c> : on hpf, then frequency gain q for Low, Mid and High
 *   EQCV<c> : source amount for Low frequency, Low gain, Mid frequency,
 *             Mid gain, High frequency, High gain and HPF
 *   EQ<c>Z<z>, EQCV<c>Z<z> : the same for a zone with its own EQ
 *   Grain : channel size overlap spray detune octave reverse pan shape scan slide skew
 *   GrainCV : source amount for each route
 *
 * Channels and zones count from 1 and Grain's channel 0 is off. Values are
 * the integers of eq.h and patch.h. Only lines that differ from the
 * defaults are written, so a preset without extras is the stock file. */

#define PRESET_LIBRARY_BASE       0x80116bccu
#define PRESET_RECORD_BYTES       0x1660u   /* PRESET_RECORD_SIZE */
#define PRESET_LIBRARY_RECORDS    200u
#define PRESET_YAML_STATE_MODEL   0x0cu
#define STATUS_BYTE_MASK          0xffu
#define CHANNELS                  8u
#define ZONES                     8u
#define NO_ZONE                   ZONES
#define EQ_NUMBERS                11u       /* on, hpf, three bands of three */
#define NUMBERS_MAX               16u
#define LINE_BYTES                112u
#define KEY_BYTES                 12u

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
/* A slot per library record that has extras. Slots are allocated in the
 * foreground: while a folder's presets are parsed, and one kept ready for a
 * load or Save, which the front panel runs in its interrupt. */
static struct extras *library[PRESET_LIBRARY_RECORDS], *spare;

const struct grain_range grain_ranges[GRAIN_FIELDS] = {
    {0, CHANNELS, 0},   /* the channel playing grains */
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
    for (u32 ch = 0; ch < CHANNELS; ++ch) eq_default(&e->channels[ch]);
    for (u32 f = 0; f < GRAIN_FIELDS; ++f) e->grain.value[f] = grain_ranges[f].initial;
    for (u32 r = 0; r < GRAIN_ROUTES; ++r) {
        e->grain.source[r] = EQ_CV_SOURCE_OFF;
        e->grain.amount[r] = EQ_CV_AMOUNT_MAX;
    }
}

/* The live settings become a slot's, or the defaults. */
static void load(const struct extras *e)
{
    if (e) patch_live = *e; else defaults(&patch_live);
    eq_changed();
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
        v->frequency[band] = (u16)clamp(n[2+3*band], EQ_FREQUENCY_MIN, EQ_FREQUENCY_MAX);
        v->gain[band] = (signed short)clamp(n[3+3*band], -EQ_GAIN_MAX, EQ_GAIN_MAX);
        v->q[band] = (u8)clamp(n[4+3*band], EQ_Q_OWN, EQ_Q_STEPS);
    }
    eq_limit(v);
}
static void get_eq(const struct eq_value *v, s32 *n)
{
    n[0] = v->on; n[1] = v->hpf;
    for (u32 band = 0; band < 3; ++band) {
        n[2+3*band] = v->frequency[band]; n[3+3*band] = v->gain[band]; n[4+3*band] = v->q[band];
    }
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

/* One line of a preset file: store it when it is one of ours and complete. */
static void line(u32 model, const char *s)
{
    while (*s == ' ' || *s == '\t') ++s;
    u32 grain = !strncasecmp(s, "Grain", 5);
    if (!grain && strncasecmp(s, "EQ", 2)) return;
    s += grain ? 5 : 2;
    u32 cv = !strncasecmp(s, "CV", 2), channel = 0, zone = NO_ZONE;
    if (cv) s += 2;
    if (!grain) {
        channel = (u32)(*s++ - '1');
        if (channel >= CHANNELS) return;
        if (*s == 'Z' || *s == 'z') {
            zone = (u32)(s[1] - '1');
            if (zone >= ZONES) return;
            s += 2;
        }
    }
    u32 wanted = cv ? 2*(grain ? GRAIN_ROUTES : EQ_MOD_COUNT) : grain ? GRAIN_FIELDS : EQ_NUMBERS, count = 0;
    s32 n[NUMBERS_MAX];
    while (count < wanted && (s = number(s, &n[count]))) ++count;
    if (count != wanted) return;
    struct extras *e = destination(model);
    if (!e) return;
    if (grain) {
        if (cv) put_routes(e->grain.source, e->grain.amount, n, GRAIN_ROUTES);
        else for (u32 f = 0; f < GRAIN_FIELDS; ++f)
            e->grain.value[f] = (s16)clamp(n[f], grain_ranges[f].low, grain_ranges[f].high);
        return;
    }
    struct eq_value *v = zone == NO_ZONE ? &e->channels[channel] : &e->zones[channel][zone];
    if (zone != NO_ZONE && !v->override) { eq_default(v); v->override = 1; }
    if (cv) put_routes(v->source, v->amount, n, EQ_MOD_COUNT); else put_eq(v, n);
}

static void write_line(FIL *file, const char *key, const s32 *n, u32 count)
{
    char text[LINE_BYTES];
    u32 done, at = (u32)sprintf(text, "  %s :", key);
    for (u32 i = 0; i < count; ++i) at += (u32)sprintf(text + at, " %d", n[i]);
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
        write_line(file, key, n, EQ_NUMBERS);
    }
    if (get_routes(v->source, v->amount, n, EQ_MOD_COUNT)) {
        sprintf(key, "EQCV%d%s", channel + 1, suffix);
        write_line(file, key, n, 2*EQ_MOD_COUNT);
    }
}

static void append(const char *path, const struct extras *e)
{
    FIL file;
    s32 n[NUMBERS_MAX];
    if (fatfs_open(&file, path, FA_WRITE)) return;
    if (!resource_seek_absolute(&file, file.words[FIL_FILE_SIZE / 4])) {
        for (u32 ch = 0; ch < CHANNELS; ++ch) {
            write_eq(&file, &e->channels[ch], ch, NO_ZONE);
            for (u32 z = 0; z < ZONES; ++z)
                if (e->zones[ch][z].override) write_eq(&file, &e->zones[ch][z], ch, z);
        }
        u32 differs = 0;
        for (u32 f = 0; f < GRAIN_FIELDS; ++f) {
            n[f] = e->grain.value[f];
            differs |= n[f] != grain_ranges[f].initial;
        }
        if (differs) write_line(&file, "Grain", n, GRAIN_FIELDS);
        if (get_routes(e->grain.source, e->grain.amount, n, GRAIN_ROUTES)) write_line(&file, "GrainCV", n, 2*GRAIN_ROUTES);
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
